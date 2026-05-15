#include "cxl/cxl_base.h"

#include "spdlog/spdlog.h"

namespace dfs {

CXLPointer::CXLPointer() { offset_ = 0; }

CXLPointer::CXLPointer(uint64_t offset) { offset_ = offset; }

CXLPointer::CXLPointer(const CXLBuffer &buf) { offset_ = buf.base_offset_; }

void CXLPointer::SetNullptr() { offset_ = 0; }

auto CXLPointer::IsNullptr() -> bool { return offset_ == 0; }

CXLBuffer::CXLBuffer() {
  base_offset_ = 0;
  size_ = 0;
}

CXLBuffer::CXLBuffer(uint64_t base, uint64_t size) {
  base_offset_ = base;
  size_ = size;
};

auto CXLBuffer::GetSubbuf(uint64_t offset, uint64_t size) -> CXLBuffer {
  if (offset + size > size_) {
    SPDLOG_ERROR("out of memory! base_offset = {}, "
                 "base_size = {}, "
                 "offset = {}, size = {}",
                 base_offset_, size_, offset, size);
  }
  return {base_offset_ + offset, size};
}

auto CXLBuffer::At(uint64_t offset) -> CXLPointer {
  if (offset > size_) {
    SPDLOG_ERROR("out of memory! base_size = {}, offset = {}", size_, offset);
  }
  return CXLPointer(base_offset_ + offset);
}

void CXLBuffer::SetNullptr() {
  base_offset_ = 0;
  size_ = 0;
}

auto CXLBuffer::IsNullptr() -> bool { return base_offset_ == 0; }

} // namespace dfs