#include "cxl/gim_mem.h"

#include "spdlog/spdlog.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <numaif.h>
#include <sys/file.h>
#include <sys/mman.h>

namespace dfs {

// GIM VA base: 0x700000000000 (separate from CXL at 0x600000000000)
static constexpr uintptr_t kGIMBaseAddr = 0x0000700000000000;

auto GIMMem::Init(int meta_num, uint64_t cxl_capacity, uint64_t per_meta_MiB,
                  const std::string &hugepage_base_path,
                  int numa_node) -> int {
  assert(meta_num >= 0 && meta_num < kMaxMetaNum);

  my_meta_id_ = meta_num;
  per_meta_size_ = per_meta_MiB * 1024ULL * 1024ULL;
  cxl_capacity_ = cxl_capacity;
  hugepage_base_path_ = hugepage_base_path;

  // Per-meta hugepage file
  std::string gim_path = hugepage_base_path + "_" + std::to_string(meta_num);
  SPDLOG_INFO("GIMMem: meta {} path={}, capacity={}MB, numa_node={}", meta_num,
              gim_path, per_meta_MiB, numa_node);

  // Allocate hugepages with file lock (avoid race between metas)
  std::string sys_path = "/sys/devices/system/node/node" +
                         std::to_string(numa_node) +
                         "/hugepages/hugepages-2048kB/nr_hugepages";
  uint64_t increase_pages = per_meta_size_ / (2048 * 1024);

  std::string lock_path =
      "/var/lock/hugepage_lock_" + std::to_string(numa_node);
  int fd_lock = open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
  if (fd_lock < 0) {
    SPDLOG_ERROR("GIMMem: failed to create lock for node {}", numa_node);
    return -1;
  }

  if (flock(fd_lock, LOCK_EX) != 0) {
    SPDLOG_ERROR("GIMMem: flock failed on node {}", numa_node);
    close(fd_lock);
    return -1;
  }

  uint64_t current = 0;
  {
    std::ifstream in(sys_path);
    if (in.is_open())
      in >> current;
  }
  {
    std::ofstream out(sys_path);
    if (out.is_open())
      out << (current + increase_pages);
  }

  flock(fd_lock, LOCK_UN);
  close(fd_lock);

  SPDLOG_INFO("GIMMem: node {} hugepages {} -> {}", numa_node, current,
              current + increase_pages);

  // Open hugepage file
  int fd = open(gim_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    SPDLOG_ERROR("GIMMem: open {} failed: {}", gim_path, strerror(errno));
    return -1;
  }
  if (ftruncate(fd, per_meta_size_) < 0) {
    SPDLOG_ERROR("GIMMem: ftruncate failed: {}", strerror(errno));
    close(fd);
    return -1;
  }

  // mmap at fixed VA: GIM_BASE + meta_num * per_meta_size
  uintptr_t my_addr = kGIMBaseAddr + meta_num * per_meta_size_;
  void *start_addr = reinterpret_cast<void *>(my_addr);

  const uint64_t nodemask = 1ULL << numa_node;
  set_mempolicy(MPOL_BIND, &nodemask, 64);

  const int mmap_prot = PROT_READ | PROT_WRITE;
  const int mmap_flags = MAP_FIXED | MAP_SHARED | MAP_POPULATE | MAP_HUGETLB;
  auto *mmap_buf =
      static_cast<uint8_t *>(mmap(start_addr, per_meta_size_, mmap_prot,
                                  mmap_flags, fd, 0));

  set_mempolicy(MPOL_DEFAULT, nullptr, 0);
  close(fd);

  if (mmap_buf == MAP_FAILED) {
    SPDLOG_ERROR("GIMMem: mmap failed at {}: {}", fmt::ptr(start_addr),
                 strerror(errno));
    return -1;
  }

  if (mbind(mmap_buf, per_meta_size_, MPOL_BIND, &nodemask, 64, 0) != 0) {
    SPDLOG_ERROR("GIMMem: mbind failed: {}", strerror(errno));
  }

  // Zero own region
  ::memset(mmap_buf, 0, per_meta_size_);
  SPDLOG_INFO("GIMMem: meta {} mmap'd at {}, {}MB on node {}",
              meta_num, fmt::ptr(mmap_buf), per_meta_MiB, numa_node);

  my_buf_ = mmap_buf;
  meta_bufs_[meta_num] = mmap_buf;

  return 0;
}

void GIMMem::MapOtherMetas(const int *numa_nodes) {
  for (int i = 0; i < total_metas_; i++) {
    if (i == my_meta_id_)
      continue;

    std::string gim_path = hugepage_base_path_ + "_" + std::to_string(i);
    int fd = open(gim_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
      SPDLOG_ERROR("GIMMem: open {} failed: {}", gim_path, strerror(errno));
      continue;
    }
    if (ftruncate(fd, per_meta_size_) < 0) {
      SPDLOG_ERROR("GIMMem: ftruncate failed: {}", strerror(errno));
      close(fd);
      continue;
    }

    uintptr_t addr = kGIMBaseAddr + i * per_meta_size_;
    void *start_addr = reinterpret_cast<void *>(addr);

    const int mmap_prot = PROT_READ | PROT_WRITE;
    const int mmap_flags = MAP_FIXED | MAP_SHARED | MAP_POPULATE | MAP_HUGETLB;
    auto *mmap_buf =
        static_cast<uint8_t *>(mmap(start_addr, per_meta_size_, mmap_prot,
                                    mmap_flags, fd, 0));
    close(fd);

    if (mmap_buf == MAP_FAILED) {
      SPDLOG_ERROR("GIMMem: mmap other meta {} failed: {}", i,
                   strerror(errno));
      continue;
    }

    // mbind to the other meta's NUMA node
    if (numa_nodes) {
      int other_numa = numa_nodes[i];
      const uint64_t mask = 1ULL << other_numa;
      mbind(mmap_buf, per_meta_size_, MPOL_BIND, &mask, 64, 0);
    }

    meta_bufs_[i] = mmap_buf;
    SPDLOG_INFO("GIMMem: mapped meta {} at {} (node {})", i,
                fmt::ptr(mmap_buf), numa_nodes ? numa_nodes[i] : -1);
  }
}

GIMMem::~GIMMem() {
  // Don't munmap — shared memory regions should persist across server lifetime
}

} // namespace dfs
