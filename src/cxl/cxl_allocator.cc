#include "cxl/cxl_allocator.h"

#include "fmt/format.h"
#include "spdlog/spdlog.h"

namespace dfs {

void NaiveAllocator::Init(void *buf, uint64_t capacity) {
  if (magic_num_ != 90801928124ULL) {
    buf_ = static_cast<uint8_t *>(buf);
    capacity_ = capacity;
    offset_.store(0);
    free_size_.store(0);
    magic_num_ = 90801928124ULL;
  }
}

auto NaiveAllocator::CXLMalloc(size_t size) -> void * {
  SPDLOG_TRACE("cxl malloc size: {}", size);
  // make size is cacheline aligned
  size = ((size + 64 - 1) / 64) * 64;

  uint64_t old_offset = offset_.fetch_add(size);
  uint64_t new_offset = old_offset + size;
  if (new_offset > capacity_) {
    SPDLOG_ERROR("out of memory! capacity = {}, offset = {}, size = {}",
                 capacity_, old_offset, size);
    std::exit(1);
    return nullptr;
  }
  return buf_ + old_offset;
}

auto NaiveAllocator::CXLMallocOffset(size_t size) -> uint64_t {
  SPDLOG_TRACE("cxl malloc size: {}", size);
  // make size is cacheline aligned
  size = ((size + 64 - 1) / 64) * 64;

  uint64_t old_offset = offset_.fetch_add(size);
  uint64_t new_offset = old_offset + size;
  if (new_offset > capacity_) {
    SPDLOG_ERROR("out of memory! capacity = {}, offset = {}, size = {}",
                 capacity_, old_offset, size);
    std::exit(1);
    return 0;
  }
  return old_offset;
}

void NaiveAllocator::CXLFree(void *ptr, size_t size) {
  if (ptr == nullptr) {
    return;
  }
  if (ptr < buf_ || ptr > buf_ + capacity_ - size) {
    SPDLOG_ERROR(
        "free invalid pointer: ptr = {}, size = {}, buf = {}, capacity = {}",
        fmt::ptr(ptr), size, fmt::ptr(buf_), capacity_);
    return;
  }
  size = ((size + 64 - 1) / 64) * 64;
  free_size_.fetch_add(size);
}

void NaiveAllocator::CXLFakeFree(void *ptr, size_t size) {
  if (ptr == nullptr) {
    return;
  }
  size = ((size + 64 - 1) / 64) * 64;
  if (ptr < buf_ || ptr > buf_ + capacity_ - size) {
    SPDLOG_ERROR(
        "free invalid pointer: ptr = {}, size = {}, buf = {}, capacity = {}",
        fmt::ptr(ptr), size, fmt::ptr(buf_), capacity_);
    return;
  }
  free_size_.fetch_add(size);
}

} // namespace dfs