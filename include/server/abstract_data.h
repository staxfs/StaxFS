#pragma once

#include <cstdint>

namespace dfs {

struct ChunkId {
public:
  uint64_t id_;
};

struct Status {
  int ec_;
};

struct AsyncContext {};

using ChunkIOCallback = void(AsyncContext *, Status, std::size_t);

class AbstractDataStorage {
public:
  virtual ~AbstractDataStorage() = 0;
  virtual void AsyncReadChunk(AsyncContext *, ChunkId, void *buffer,
                              std::size_t offset, std::size_t length,
                              ChunkIOCallback) = 0;
  virtual void AsyncWriteChunk(AsyncContext *, ChunkId, void const *buffer,
                               std::size_t offset, std::size_t length,
                               ChunkIOCallback) = 0;
};

} // namespace dfs