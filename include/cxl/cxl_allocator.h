#pragma once

#include <atomic>

namespace dfs {

// shareable
class NaiveAllocator {
public:
  uint64_t magic_num_;
  uint8_t *buf_;
  uint64_t capacity_;
  std::atomic_uint64_t offset_;
  std::atomic_uint64_t free_size_;

  NaiveAllocator() = default;
  void Init(void *buf, uint64_t capacity);
  auto CXLMalloc(size_t size) -> void *;
  auto CXLMallocOffset(size_t size) -> uint64_t;
  void CXLFree(void *ptr, size_t size);

  // No reused this memory after free
  void CXLFakeFree(void *ptr, size_t size);
};

} // namespace dfs