#pragma once

#include <cstdint>

namespace dfs {

class CXLBuffer;

class CXLPointer {
public:
  uint64_t offset_;

  CXLPointer();
  explicit CXLPointer(uint64_t offset);
  explicit CXLPointer(const CXLBuffer &buf);
  void SetNullptr();
  auto IsNullptr() -> bool;
};

class CXLBuffer {
public:
  uint64_t base_offset_;
  uint64_t size_;

  CXLBuffer();
  CXLBuffer(uint64_t base, uint64_t size);
  auto GetSubbuf(uint64_t offset, uint64_t size) -> CXLBuffer;
  auto At(uint64_t offset) -> CXLPointer;
  void SetNullptr();
  auto IsNullptr() -> bool;
};

} // namespace dfs
