#include "cxl/gim_context.h"

#include "spdlog/spdlog.h"
#include <fcntl.h>
#include <fstream>
#include <numaif.h>
#include <sys/file.h>
#include <sys/mman.h>

namespace dfs {

void GIMGlobalHeader::Init() {
  if (magic_num_ != 0x8767987a) {
    SPDLOG_INFO("Init GIMGlobalHeader");
    magic_num_ = 0x8767987a;
  }
}

auto GIMGlobalHeader::IsInit() -> bool { return magic_num_ == 0x8767987a; }

GIMContext::GIMContext(uint64_t cxl_capacity, int meta_num,
                       const std::string &gim_memory_path,
                       uint64_t gim_capacity, int numa_node) {
  capacity_ = gim_capacity;
  cxl_capacity_ = cxl_capacity;
  meta_num_ = meta_num;
  huge_mem_path_ = gim_memory_path;

  std::string gim_path = gim_memory_path + "-" + std::to_string(meta_num);
  const char *huge_mem_path = gim_path.c_str();
  SPDLOG_INFO("GIM memory path: {}", gim_path);
  SPDLOG_INFO("GIM memory capacity: {} MB", gim_capacity / 1024 / 1024);

  std::string path = "/sys/devices/system/node/node" +
                     std::to_string(numa_node) +
                     "/hugepages/hugepages-2048kB/nr_hugepages";
  size_t increase_pages = gim_capacity / (2048 * 1024);

  std::string lock_path =
      "/var/lock/hugepage_lock_" + std::to_string(numa_node);
  int fd_lock = open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
  if (fd_lock < 0) {
    SPDLOG_ERROR("Initialize GIM: Failed to create lock for NUMA node {}",
                 numa_node);
    exit(-1);
  }

  if (flock(fd_lock, LOCK_EX) != 0) {
    SPDLOG_ERROR("Initialize GIM: flock lock failed on NUMA node {}",
                 numa_node);
    close(fd_lock);
    exit(-1);
  }

  size_t current = 0;
  {
    std::ifstream in(path);
    if (!in.is_open()) {
      SPDLOG_ERROR("Initialize GIM: Failed to read hugepages on NUMA node {}",
                   numa_node);
      flock(fd_lock, LOCK_UN);
      close(fd_lock);
      exit(-1);
    }
    in >> current;
  }

  size_t target = current + increase_pages;
  {
    std::ofstream out(path);
    if (!out.is_open()) {
      SPDLOG_ERROR("Initialize GIM: Failed to write hugepages on NUMA node {}",
                   numa_node);
      flock(fd_lock, LOCK_UN);
      close(fd_lock);
      exit(-1);
    }
    out << target;
  }

  flock(fd_lock, LOCK_UN);
  close(fd_lock);

  SPDLOG_INFO("Initialize GIM: Change numa_node {} huge page to {}", numa_node,
              target);

  int fd = open(huge_mem_path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    SPDLOG_ERROR("Initialize GIM: open failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }
  if (ftruncate(fd, gim_capacity) < 0) {
    SPDLOG_ERROR("Initialize GIM: ftruncate failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }

  uintptr_t k_base_addr = 0x0000600000000000;
  k_base_addr += cxl_capacity + meta_num * gim_capacity;
  void *start_addr = reinterpret_cast<void *>(k_base_addr);

  // https://man7.org/linux/man-pages/man2/mmap.2.html
  const int mmap_port = PROT_READ | PROT_WRITE | PROT_EXEC;
  const int mmap_flags = MAP_FIXED | MAP_SHARED | MAP_POPULATE | MAP_HUGETLB;
  auto *mmap_buf = static_cast<uint8_t *>(
      mmap(start_addr, gim_capacity, mmap_port, mmap_flags, fd, 0));
  if (mmap_buf == MAP_FAILED) {
    SPDLOG_ERROR("Initialize GIM: mmap failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }
  if (mmap_buf != start_addr) {
    SPDLOG_ERROR("Initialize GIM: mmap failed, address do not match");
    exit(-1);
  }

  // bind to numa 5, const uint64_t mask = 0b00100000;
  const uint64_t mask = static_cast<uint64_t>(1) << numa_node;
  SPDLOG_INFO("Initialize GIM: bind to numa_node {}, mask = {}", numa_node,
              mask);
  // https://linux.die.net/man/2/mbind
  int status = mbind(mmap_buf, gim_capacity, MPOL_BIND, &mask, 64, 0);
  if (status != 0) {
    SPDLOG_ERROR("Initialize GIM: mbind failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }

  buf_ptr_ = mmap_buf;
}

void GIMContext::GIMReset() {
  ::memset(buf_ptr_, 0, capacity_);
  SPDLOG_INFO("memset gim memory region");
}

void GIMContext::MapOtherGIMMemory() {
  for (int i = 0; i < gim_header_->meta_counts_.load(); i++) {
    if (i != meta_num_) {
      std::string gim_path = huge_mem_path_ + "-" + std::to_string(i);
      const char *huge_mem_path = gim_path.c_str();
      int fd = open(huge_mem_path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
      if (fd < 0) {
        SPDLOG_ERROR("Initialize GIM: open failed, errno: {}",
                     std::strerror(errno));
        exit(-1);
      }
      if (ftruncate(fd, capacity_) < 0) {
        SPDLOG_ERROR("Initialize GIM: ftruncate failed, errno: {}",
                     std::strerror(errno));
        exit(-1);
      }

      uintptr_t k_base_addr = 0x0000600000000000;
      k_base_addr += cxl_capacity_ + i * capacity_;
      void *start_addr = reinterpret_cast<void *>(k_base_addr);

      // https://man7.org/linux/man-pages/man2/mmap.2.html
      const int mmap_port = PROT_READ | PROT_WRITE | PROT_EXEC;
      const int mmap_flags =
          MAP_FIXED | MAP_SHARED | MAP_POPULATE | MAP_HUGETLB;
      auto *mmap_buf = static_cast<uint8_t *>(
          mmap(start_addr, capacity_, mmap_port, mmap_flags, fd, 0));
      if (mmap_buf == MAP_FAILED) {
        SPDLOG_ERROR("Initialize GIM: mmap failed, errno: {}",
                     std::strerror(errno));
        exit(-1);
      }
      if (mmap_buf != start_addr) {
        SPDLOG_ERROR("Initialize GIM: mmap failed, address do not match");
        exit(-1);
      }

      // NOTE: GIMContext is legacy code; NUMA node info now lives in DFSHeader.
      // Default to node 0 to keep this compilable.
      const uint64_t mask = static_cast<uint64_t>(1) << 0;
      // https://linux.die.net/man/2/mbind
      int status = mbind(mmap_buf, capacity_, MPOL_BIND, &mask, 64, 0);
      if (status != 0) {
        SPDLOG_ERROR("Initialize GIM: mbind failed, errno: {}",
                     std::strerror(errno));
        exit(-1);
      }
    }
  }
}

} // namespace dfs