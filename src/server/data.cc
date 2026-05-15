#include "server/data.h"

#include <asm-generic/errno-base.h>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <filesystem>
#include <unistd.h>

namespace dfs {

auto ObjectStore::Init(std::string_view data_root)
    -> std::unique_ptr<ObjectStore> {
  auto store = std::unique_ptr<ObjectStore>(new ObjectStore);
  store->backend_store_ = std::make_unique<BackendStore>();
  std::filesystem::create_directories(data_root);
  SPDLOG_INFO("Data root path created: {}", data_root);
  return store;
}

/*
  * Read the block data from the backend store and copy it to the buffer.
  * If the offset exceeds the block size, return error.
  @return: >=0 on success, -1 on failure
*/
auto ObjectStore::Read(blkuuid_t blkid, size_t offset, size_t size,
                       char *buffer) -> ssize_t {

  DTRACE_PROBE3(DFS_SERVER, ObjectStore_Read_enter, blkid, offset, size);
  SPDLOG_TRACE("Read operation: blkid: {}, offset: {}, size: {}", blkid, offset,
               size);
  if (buffer == nullptr) {
    SPDLOG_ERROR("Read operation buffer is nullptr");
    return -1;
  }
  
  ssize_t ret = 0;
  bool find_ret = backend_store_->find_fn(
      blkid, [offset, size, buffer, &ret](const std::vector<char> &blk_data) {
        if (offset > blk_data.size()) {
          SPDLOG_ERROR("Read operation offset exceeds block size");
          ret = -1;
          return;
        }

        ret = std::min(size, blk_data.size() - offset);
        std::copy(blk_data.begin() + offset, blk_data.begin() + offset + ret,
                  buffer);
      });
  // can not find the block
  if (!find_ret) {
    SPDLOG_ERROR("Read operation can not find the block");
    return -1;
  }
  DTRACE_PROBE1(DFS_SERVER, ObjectStore_Read_ret, ret);
  return ret;
}

/*
  * Write the block data to the backend store.
  * If the offset exceeds the block size, resize the block.
  @return: >=0 on success, -1 on failure
*/
auto ObjectStore::Write(blkuuid_t blkid, size_t offset, size_t size,
                        const char *buffer) -> ssize_t {
  DTRACE_PROBE3(DFS_SERVER, ObjectStore_Write_enter, blkid, offset, size);
  SPDLOG_TRACE("Write operation: blkid: {}, offset: {}, size: {}", blkid,
               offset, size);
  if (buffer == nullptr) {
    SPDLOG_ERROR("Write operation buffer is nullptr");
    return -1;
  }
  ssize_t ret = 0;
  bool insert_ret = backend_store_->upsert(
      blkid,
      [offset, size, buffer, &ret](std::vector<char> &blk_data, auto...) {
        if (blk_data.size() < offset + size) {
          SPDLOG_TRACE("Write operation resize the block");
          blk_data.resize(offset + size);
        }

        std::copy(buffer, buffer + size, blk_data.begin() + offset);
        ret = size;
      },
      std::vector<char>());
  // can not find the block
  if (insert_ret) {
    SPDLOG_TRACE("Write operation can not find the block, create a new block");
  }

  DTRACE_PROBE1(DFS_SERVER, ObjectStore_Write_ret, ret);
  return ret;
}

}; // namespace dfs
