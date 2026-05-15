#pragma once
#include "common/metadata_types.h"
#include "config.h"
#include "data_generated.h"
#include "flatbuffers/flatbuffer_builder.h"
#include "libcuckoo/cuckoohash_map.hh"
#include "mdrequest_generated.h"
#include "msg_buffer.h"
#include "nexus.h"
#include "transport_impl/infiniband/ib_transport.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <rpc.h>
#include <spdlog/spdlog.h>
#include <string>

namespace dfs {

using libcuckoo::cuckoohash_map;
using mdrequest::MDOpType::MDOpType_Access;
using mdrequest::MDOpType::MDOpType_Chmod;
using mdrequest::MDOpType::MDOpType_Create;
using mdrequest::MDOpType::MDOpType_Link;
using mdrequest::MDOpType::MDOpType_MakeDir;
using mdrequest::MDOpType::MDOpType_OpenDir;
using mdrequest::MDOpType::MDOpType_RemoveDir;
using mdrequest::MDOpType::MDOpType_Rename;
using mdrequest::MDOpType::MDOpType_Stat;
using mdrequest::MDOpType::MDOpType_Unlink;

// Thread shared
class SharedContext {
public:
  std::shared_ptr<erpc::Nexus> nexus_;
  int node_rank_;
  std::atomic_int num_rpcs_;
  std::vector<int> data_threads_;
  std::vector<std::string> data_uri_list_;
  std::vector<int> meta_threads_;
  std::vector<std::string> meta_uri_list_;

  explicit SharedContext(std::string const &config_file_path);

  void AddDataServer(const std::string &server_uri, int threads = 1);

  void AddMetaServer(std::string const &server_uri, int threads = 1);

  ~SharedContext() = default;
};

class ClientRpcWrapper {
  std::vector<std::vector<int>> data_sessions_;
  std::vector<std::vector<int>> meta_sessions_;
  erpc::FastRand fastrand_;

  struct MapInfo {
    int16_t meta_num_;
    std::string new_path_;

    MapInfo() = default;

    explicit MapInfo(int16_t meta_num, std::string new_path)
        : meta_num_(meta_num), new_path_(std::move(new_path)) {}
  };

  cuckoohash_map<std::string, MapInfo> path_cache_;

  // Use Lemire's trick to get a random session number from session_num_vec
  inline auto FastRandSessionNum(const std::vector<int> &sn_vec) -> int {
    uint32_t x = fastrand_.next_u32();
    size_t rand_index = (static_cast<size_t>(x) * sn_vec.size()) >> 32;
    return sn_vec[rand_index];
  }

  inline auto FastRandSessionNum(const int range) -> int {
    uint32_t x = fastrand_.next_u32();
    return (static_cast<size_t>(x) * range) >> 32;
  }

  uint64_t LoadLastSeenHLC() const {
    return last_seen_hlc_.load(std::memory_order_acquire);
  }

  void ObserveServerHLC(uint64_t hlc) {
    uint64_t old = last_seen_hlc_.load(std::memory_order_relaxed);
    while (hlc > old && !last_seen_hlc_.compare_exchange_weak(
                            old, hlc, std::memory_order_acq_rel,
                            std::memory_order_relaxed)) {
    }
  }

public:
  std::unique_ptr<erpc::Rpc<erpc::CTransport>> rpc_;
  uint32_t uid_, gid_;
  std::atomic<uint64_t> last_seen_hlc_{0};
  explicit ClientRpcWrapper(SharedContext &shared_ctx);
  void AddDataServer(std::string const &server_uri, int dataserver,
                     int n_threads = 1);
  void AddMetaServer(std::string const &server_uri, int n_threads = 1);

  auto GetDataserverCount() -> int;

  auto GetDataserverThreadCount(int node_id) -> int;

  auto GetMetaserverCount() const -> int { return meta_sessions_.size(); }

  auto WaitServerSessionConnected() -> void;

  void RpcHelloMetaServer(int thread_id);
  void RpcHelloDataServer(int node_id, int thread_id);

  auto TestCommunication() -> void;

  // Data RPC APIs
  auto RpcPwrite(int node_id, uint64_t objuuid, uint64_t offset,
                 uint64_t io_size, const void *io_buffer) -> int;
  auto RpcPread(int node_id, uint64_t objuuid, uint64_t offset,
                uint64_t io_size, void *io_buffer) -> int;

  // Metadata RPC APIs
  auto MetaCall(flatbuffers::FlatBufferBuilder &fbb, int req_type, int meta_num,
                int resp_size = 1) -> erpc::MsgBuffer;

  auto PathCommonRequest(const char *path, mdrequest::MDOpType op,
                         uint32_t mode = 0, void *buf = nullptr) -> int;
  auto FDCommonRequest(uint64_t id, mdrequest::MDOpType op, off_t offset = 0,
                       uint32_t u32arg = 0, void *buf = nullptr,
                       int meta_num = -1, uint64_t u64arg = 0) -> int;
  // Issue N GetDentViews requests in parallel (one per entry in meta_nums /
  // offsets). After all responses arrive, the payload for request i (HLC +
  // status + length header stripped) is memcpy'd into out_bufs[i] and the
  // byte count is written to out_sizes[i]. Individual per-meta errors are
  // reported as out_sizes[i] = -1; the function return is 0 iff every
  // request succeeded.
  auto BatchGetDentViews(uint64_t id, uint64_t read_cutoff_version, int n,
                         const int *meta_nums, const off_t *offsets,
                         char *const *out_bufs, uint32_t chunk_size,
                         int *out_sizes) -> int;
  auto GetInodeRequest(uint64_t inode_id, dfs::Inode *i_buf) -> int;
  auto PutInodeRequest(uint64_t inode_id, const dfs::Inode *i_buf) -> int;

  // Adapt CommonPathRequest to fit each syscall interface.
  inline auto Stat(const char *path, dfs::Inode *buf) -> int {
    return PathCommonRequest(path, MDOpType_Stat, 0, buf);
  };

  inline auto Unlink(const char *path) -> int {
    return PathCommonRequest(path, MDOpType_Unlink);
  };

  inline auto Rmdir(const char *path) -> int {
    return PathCommonRequest(path, MDOpType_RemoveDir);
  };

  inline auto Access(const char *path, uint32_t mode) -> int {
    return PathCommonRequest(path, MDOpType_Access, mode);
  };

  inline auto Mkdir(const char *path, uint32_t mode) -> int {
    return PathCommonRequest(path, MDOpType_MakeDir, mode);
  };

  inline auto Rename(const char *oldpath, const char *newpath) -> int {
    return PathCommonRequest(oldpath, MDOpType_Rename, 0, (void *)newpath);
  };

  inline auto Link(const char *oldpath, const char *newpath) -> int {
    return PathCommonRequest(oldpath, MDOpType_Link, 0, (void *)newpath);
  };

  inline auto Create(const char *path, uint32_t mode, dfs::Inode *buf) -> int {
    return PathCommonRequest(path, MDOpType_Create, mode, buf);
  };

  inline auto Chmod(const char *path, uint32_t mode) -> int {
    return PathCommonRequest(path, MDOpType_Chmod, mode);
  };

  inline auto OpenDir(const char *path, DirHandle *handle) {
    return PathCommonRequest(path, MDOpType_OpenDir, 0, handle);
  };

  inline auto GetDents(uint64_t id, off_t offset, void *buf, uint32_t size,
                       int meta_num = -1) -> int {
    return FDCommonRequest(id, mdrequest::MDOpType_GetDents, offset, size, buf,
                           meta_num);
  };

  inline auto GetDentViews(uint64_t id, uint64_t read_cutoff_version,
                           off_t offset, void *buf, uint32_t size,
                           int meta_num) -> int {
    return FDCommonRequest(id, mdrequest::MDOpType_GetDentViews, offset, size,
                           buf, meta_num, read_cutoff_version);
  };

  ~ClientRpcWrapper();
};

} // namespace dfs
