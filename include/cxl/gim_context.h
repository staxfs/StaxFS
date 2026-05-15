#pragma once

#include "cxl/cxl_allocator.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace dfs {

// Per-MDS resize notification in GIM local memory.
// Initiator writes version here via GIMWriteSync (~1.27μs RDMA Write).
// Each MDS polls its own copy via local DRAM read (~6ns).
// 64B aligned because GIM RemoteWrite operates in 64B chunks.
struct alignas(64) GIMResizeNotify {
  uint64_t resize_version{0};
  uint8_t _pad[56];
};

struct GIMGlobalHeader {
  uint64_t magic_num_;
  std::atomic<uint64_t> meta_counts_{0};

  auto IsInit() -> bool;
  void Init();
};

class GIMContext {
public:
  uint8_t *buf_ptr_;
  uint64_t capacity_;
  uint64_t cxl_capacity_;
  int meta_num_;
  std::string huge_mem_path_;

  GIMGlobalHeader *gim_header_;
  NaiveAllocator *allocator_;

  explicit GIMContext(
      uint64_t cxl_capacity, int meta_num_,
      const std::string &gim_memory_path = "/dev/hugepages/cxl_memory",
      uint64_t gim_capacity = 16 * 1024ULL * 1024ULL, int numa_node = 0);
  ~GIMContext() = default;

  void GIMReset();
  void MapOtherGIMMemory();
};

} // namespace dfs
