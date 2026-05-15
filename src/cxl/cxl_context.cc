#include "cxl/cxl_context.h"

#include "spdlog/spdlog.h"
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>

namespace dfs {

CXLContext::CXLContext(const std::string &cxl_memory_path, uint64_t capacity,
                       int numa_node) {
  capacity_ = capacity;
  const char *huge_mem_path = cxl_memory_path.c_str();
  SPDLOG_INFO("cxl memory path: {}", cxl_memory_path);
  SPDLOG_INFO("cxl memory capacity: {} GB", capacity / 1024 / 1024 / 1024);

  // Allocate hugepages: read current count and add on top
  const uint64_t num_hugepages_needed = capacity / (2048 * 1024);
  std::string path = "/sys/devices/system/node/node" +
                     std::to_string(numa_node) +
                     "/hugepages/hugepages-2048kB/nr_hugepages";

  uint64_t current_hugepages = 0;
  std::ifstream sys_read(path);
  if (sys_read.is_open()) {
    sys_read >> current_hugepages;
    sys_read.close();
  }
  uint64_t target_hugepages = current_hugepages + num_hugepages_needed;

  std::ofstream sys_file(path);
  if (sys_file.is_open()) {
    sys_file << target_hugepages;
    sys_file.close();
    SPDLOG_INFO("Initialize CXL: node {} hugepages {} -> {} (+{})",
                numa_node, current_hugepages, target_hugepages,
                num_hugepages_needed);
  } else {
    SPDLOG_ERROR("Initialize CXL: Failed to write new hugepages value");
    exit(-1);
  }

  int fd = open(huge_mem_path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    SPDLOG_ERROR("Initialize CXL: open failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }
  if (ftruncate(fd, capacity_) < 0) {
    SPDLOG_ERROR("Initialize CXL: ftruncate failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }

  // FIXME:
  // 48 bit virtual address space
  void *start_addr = reinterpret_cast<void *>(0x0000600000000000);
  // user max_addr = (void*)0x0000800000000000;
  // 57 bit virtual address space
  // user max addr = (void*)0x0100000000000000;

  // Bind memory policy to target NUMA node BEFORE mmap, so hugepages are
  // allocated from the correct node.
  const uint64_t nodemask = static_cast<uint64_t>(1) << numa_node;
  if (set_mempolicy(MPOL_BIND, &nodemask, 64) != 0) {
    SPDLOG_ERROR("Initialize CXL: set_mempolicy failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }
  SPDLOG_INFO("Initialize CXL: set_mempolicy MPOL_BIND to numa_node {}",
              numa_node);

  // https://man7.org/linux/man-pages/man2/mmap.2.html
  const int mmap_port = PROT_READ | PROT_WRITE | PROT_EXEC;
  const int mmap_flags = MAP_FIXED | MAP_SHARED | MAP_POPULATE | MAP_HUGETLB;
  auto *mmap_buf = static_cast<uint8_t *>(
      mmap(start_addr, capacity_, mmap_port, mmap_flags, fd, 0));

  // Restore default memory policy immediately after mmap
  set_mempolicy(MPOL_DEFAULT, nullptr, 0);

  if (mmap_buf == MAP_FAILED) {
    SPDLOG_ERROR("Initialize CXL: mmap failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }
  if (mmap_buf != start_addr) {
    SPDLOG_ERROR("Initialize CXL: mmap failed, address do not match");
    exit(-1);
  }

  // mbind as additional guarantee
  SPDLOG_INFO("Initialize CXL: bind to numa_node {}, mask = {}", numa_node,
              nodemask);
  int status =
      mbind(mmap_buf, capacity_, MPOL_BIND, &nodemask, 64, MPOL_MF_MOVE);
  if (status != 0) {
    SPDLOG_ERROR("Initialize CXL: mbind failed, errno: {}",
                 std::strerror(errno));
    exit(-1);
  }

  buf_ptr_ = mmap_buf;
}

void CXLContext::CXLReset() {
  ::memset(buf_ptr_, 0, capacity_);
  // ::memset(buf_ptr_, 0, fmin(capacity_, 4 * 1024ULL * 1024ULL));
  SPDLOG_INFO("memset cxl memory region");
}

auto CXLContext::CXLMalloc(uint64_t size) -> void * {
  return allocator_->CXLMalloc(size);
}

auto CXLContext::CXLMallocCXLbuf(uint64_t size) -> CXLBuffer {
  return GetCXLbuf(allocator_->CXLMalloc(size), size);
}

void CXLContext::CXLFree(void *ptr, uint64_t size) {
  allocator_->CXLFakeFree(ptr, size);
}

void CXLContext::DumpMemoryMaps() {
  SPDLOG_INFO("Dump /proc/self/maps");
  int maps_fd = open("/proc/self/maps", O_RDONLY);
  if (maps_fd == -1) {
    SPDLOG_ERROR("open /proc/self/maps failed");
    SPDLOG_ERROR("errno : {}, {}", errno, std::strerror(errno));
    exit(-1);
  }
  char buf[1024];
  int ret = 0;
  while ((ret = read(maps_fd, buf, 1024)) > 0) {
    write(STDOUT_FILENO, buf, ret);
  }
  close(maps_fd);

  SPDLOG_INFO("Dump /proc/self/numa_maps");
  maps_fd = open("/proc/self/numa_maps", O_RDONLY);
  if (maps_fd == -1) {
    SPDLOG_ERROR("open /proc/self/maps failed");
    SPDLOG_ERROR("errno : {}, {}", errno, std::strerror(errno));
    exit(-1);
  }
  while ((ret = read(maps_fd, buf, 1024)) > 0) {
    write(STDOUT_FILENO, buf, ret);
  }
  close(maps_fd);

  SPDLOG_INFO("Dump finish");
}

CXLContext::~CXLContext() {
  cxl_header_->magic_num_ = 0;
  munmap(buf_ptr_, capacity_);
}

} // namespace dfs