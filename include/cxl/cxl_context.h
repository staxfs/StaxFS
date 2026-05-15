#pragma once

#include "cxl/cxl_allocator.h"
#include "cxl/cxl_base.h"
#include "cxl/cxl_mem.h" // CXLDFSHeader, CXLMemHeader
#include <string>

namespace dfs {

// Backward compatibility: CXLGlobalHeader is now CXLMemHeader
using CXLGlobalHeader = CXLMemHeader;

class CXLContext {
public:
  uint8_t *buf_ptr_;
  uint64_t capacity_;

  CXLGlobalHeader *cxl_header_;
  NaiveAllocator *allocator_;

  explicit CXLContext(
      const std::string &cxl_memory_path = "/dev/hugepages/cxl_memory",
      uint64_t capacity = 10 * 1024ULL * 1024ULL * 1024ULL, int numa_node = 0);
  ~CXLContext();

  void CXLReset();
  auto CXLMalloc(uint64_t size) -> void *;
  auto CXLMallocCXLbuf(uint64_t size) -> CXLBuffer;
  // No reused this memory after free
  void CXLFree(void *ptr, uint64_t size);
  void DumpMemoryMaps();

  inline auto GetPtr(CXLPointer &cxl_ptr) -> void * {
    if (cxl_ptr.IsNullptr()) {
      return nullptr;
    }
    return reinterpret_cast<void *>(buf_ptr_ + cxl_ptr.offset_);
  }

  inline auto GetPtr(CXLPointer &&cxl_ptr) -> void * {
    if (cxl_ptr.IsNullptr()) {
      return nullptr;
    }
    return reinterpret_cast<void *>(buf_ptr_ + cxl_ptr.offset_);
  }

  inline auto GetPtr(CXLBuffer cxl_buf, uint64_t offset = 0) -> void * {
    if (cxl_buf.IsNullptr()) {
      return nullptr;
    }
    return reinterpret_cast<void *>(buf_ptr_ + cxl_buf.base_offset_ + offset);
  }

  inline auto At(CXLPointer &cxl_ptr) -> void * {
    if (cxl_ptr.IsNullptr()) {
      return nullptr;
    }
    return reinterpret_cast<void *>(buf_ptr_ + cxl_ptr.offset_);
  }

  inline auto At(CXLBuffer cxl_buf, uint64_t offset = 0) -> void * {
    if (cxl_buf.IsNullptr()) {
      return nullptr;
    }
    return reinterpret_cast<void *>(buf_ptr_ + cxl_buf.base_offset_ + offset);
  }

  inline auto GetCXLptr(void *ptr) -> CXLPointer {
    return CXLPointer(
        static_cast<uint64_t>(static_cast<uint8_t *>(ptr) - buf_ptr_));
  }

  inline auto GetCXLbuf(void *ptr, uint64_t size) -> CXLBuffer {
    return {static_cast<uint64_t>(static_cast<uint8_t *>(ptr) - buf_ptr_),
            size};
  }

  inline auto IsCXLPtr(void *ptr) -> bool {
    return (buf_ptr_ <= ptr) && (ptr <= (buf_ptr_ + capacity_));
  }
};

} // namespace dfs
