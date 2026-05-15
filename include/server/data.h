#pragma once

#include "libcuckoo/cuckoohash_map.hh"
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace dfs {

using blkuuid_t = uint64_t;
using libcuckoo::cuckoohash_map;

class ObjectStore {
  // std::string data_root_path_;
  using IOCallback = void(ssize_t retval);
  // using BackendStore = std::unordered_map<blkuuid_t, std::vector<char>>;
  using BackendStore = cuckoohash_map<blkuuid_t, std::vector<char>>;

  std::unique_ptr<BackendStore> backend_store_;

  ObjectStore() = default;

public:
  static auto Init(std::string_view data_root_path)
      -> std::unique_ptr<ObjectStore>;

  auto Read(blkuuid_t blkid, size_t offset, size_t size, char *buffer)
      -> ssize_t;
  auto Write(blkuuid_t blkid, size_t offset, size_t size, char const *buffer)
      -> ssize_t;

  void AsyncRead(blkuuid_t blkid, size_t offset, size_t size, char *buffer,
                 IOCallback cb);
  void AsyncWrite(blkuuid_t blkid, size_t offset, size_t size,
                  char const *buffer, IOCallback cb);
};

} // namespace dfs