#include "cxl/cxl_mem.h"

#include "spdlog/spdlog.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <numaif.h>
#include <sys/mman.h>
#include <thread>

namespace dfs {

auto CXLMem::Init(int meta_num, uint64_t capacity_GiB, const std::string &path,
                  int numa_node) -> int {
  uint64_t GiB = 1024ULL * 1024ULL * 1024ULL;
  capacity_ = capacity_GiB * GiB;

  SPDLOG_INFO("CXLMem: path={}, capacity={}GB, numa_node={}, meta_num={}", path,
              capacity_GiB, numa_node, meta_num);

  // Only meta 0 allocates hugepages; other metas share the same file.
  if (meta_num == 0) {
    const uint64_t num_hugepages = capacity_ / (2048 * 1024);
    std::string sys_path = "/sys/devices/system/node/node" +
                           std::to_string(numa_node) +
                           "/hugepages/hugepages-2048kB/nr_hugepages";

    uint64_t current_hugepages = 0;
    std::ifstream sys_read(sys_path);
    if (sys_read.is_open()) {
      sys_read >> current_hugepages;
      sys_read.close();
    }
    uint64_t target_hugepages = current_hugepages + num_hugepages;

    std::ofstream sys_file(sys_path);
    if (sys_file.is_open()) {
      sys_file << target_hugepages;
      sys_file.close();
      SPDLOG_INFO("CXLMem: node {} hugepages {} -> {} (+{})", numa_node,
                  current_hugepages, target_hugepages, num_hugepages);
    } else {
      SPDLOG_ERROR("CXLMem: failed to write hugepages sysfs");
      return -1;
    }
  }

  // Open hugepage file (meta 0 creates, others wait for it)
  int fd = -1;
  if (meta_num == 0) {
    fd = open(path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  } else {
    // Wait for meta 0 to create the file and allocate hugepages
    for (int retry = 0; retry < 100; retry++) {
      fd = open(path.c_str(), O_RDWR);
      if (fd >= 0)
        break;
      SPDLOG_INFO("CXLMem: meta {} waiting for hugepage file...", meta_num);
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
  if (fd < 0) {
    SPDLOG_ERROR("CXLMem: open {} failed: {}", path, strerror(errno));
    return -1;
  }

  if (meta_num == 0) {
    if (ftruncate(fd, capacity_) < 0) {
      SPDLOG_ERROR("CXLMem: ftruncate failed: {}", strerror(errno));
      close(fd);
      return -1;
    }
  }

  // mmap at fixed address on target NUMA node
  void *start_addr = reinterpret_cast<void *>(0x0000600000000000);

  const uint64_t nodemask = 1ULL << numa_node;
  if (set_mempolicy(MPOL_BIND, &nodemask, 64) != 0) {
    SPDLOG_ERROR("CXLMem: set_mempolicy failed: {}", strerror(errno));
    close(fd);
    return -1;
  }

  const int mmap_prot = PROT_READ | PROT_WRITE;
  const int mmap_flags = MAP_FIXED | MAP_SHARED | MAP_POPULATE | MAP_HUGETLB;
  buf_ptr_ = static_cast<uint8_t *>(
      mmap(start_addr, capacity_, mmap_prot, mmap_flags, fd, 0));

  set_mempolicy(MPOL_DEFAULT, nullptr, 0);
  close(fd);

  if (buf_ptr_ == MAP_FAILED) {
    SPDLOG_ERROR("CXLMem: mmap failed ({}GB on node {}): {}", capacity_GiB,
                 numa_node, strerror(errno));
    buf_ptr_ = nullptr;
    return -1;
  }

  if (mbind(buf_ptr_, capacity_, MPOL_BIND, &nodemask, 64, MPOL_MF_MOVE) != 0) {
    SPDLOG_ERROR("CXLMem: mbind failed: {}", strerror(errno));
  }

  owns_mmap_ = true;

  // Meta 0 zeros the region
  if (meta_num == 0) {
    ::memset(buf_ptr_, 0, capacity_);
    SPDLOG_INFO("CXLMem: meta 0 zeroed {}GB region", capacity_GiB);
  }

  SPDLOG_INFO("CXLMem: mmap'd at {}, {}GB on node {}", fmt::ptr(buf_ptr_),
              capacity_GiB, numa_node);
  return 0;
}

CXLMem::~CXLMem() {
  if (owns_mmap_ && buf_ptr_) {
    if (header_)
      header_->magic_num_ = 0;
    munmap(buf_ptr_, capacity_);
    buf_ptr_ = nullptr;
  }
}

} // namespace dfs
