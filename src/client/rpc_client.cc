#include "client/rpc_client.h"

#include "common/dfs.h"
#include "common/metadata_types.h"
#include "data_generated.h"
#include "flatbuffers/flatbuffer_builder.h"
#include "mdfdrequest_generated.h"
#include "mdpathcommonrequest_generated.h"
#include "mdpathcommonresponse_generated.h"
#include "mdrequest_generated.h"
#include "msg_buffer.h"
#include "nexus.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <rpc.h>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/sdt.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace dfs {

struct ContContextT {
  int finished_;
  char *buf_;
};

void ContFunc(void *context, void *tag) {
  auto *ret = reinterpret_cast<ContContextT *>(tag);
  SPDLOG_TRACE("Receive response");
  ret->finished_ = 1;
}

thread_local std::atomic<uint64_t> gConnectedSessionNum(0);

void SessionHandler(int session_num, erpc::SmEventType event_type,
                    erpc::SmErrType err_type, void * /*unused*/) {
  switch (event_type) {
  case erpc::SmEventType::kConnected:
    gConnectedSessionNum.fetch_add(1);
    SPDLOG_TRACE("DFS client session {} connected", session_num);
    break;
  case erpc::SmEventType::kConnectFailed:
    SPDLOG_ERROR("DFS client session {} connect failed: {}. Aborting.",
                 session_num, erpc::sm_err_type_str(err_type));
    std::abort();
  case erpc::SmEventType::kDisconnected:
    SPDLOG_TRACE("DFS client session {} disconnected", session_num);
    break;
  case erpc::SmEventType::kDisconnectFailed:
    SPDLOG_ERROR("DFS client session {} disconnect failed: {}", session_num,
                 erpc::sm_err_type_str(err_type));
    break;
  }
}

void SharedContext::AddDataServer(const std::string &server_uri, int threads) {
  data_uri_list_.push_back(server_uri);
  data_threads_.push_back(threads);
};

void SharedContext::AddMetaServer(const std::string &server_uri, int threads) {
  meta_uri_list_.push_back(server_uri);
  meta_threads_.push_back(threads);
};

ClientRpcWrapper::ClientRpcWrapper(SharedContext &shared_ctx) {
  uid_ = 1; // We just hardcode it for now.
  gid_ = 2;
  rpc_ = std::make_unique<erpc::Rpc<erpc::CTransport>>(
      shared_ctx.nexus_.get(), nullptr, ++shared_ctx.num_rpcs_, SessionHandler);
  SPDLOG_INFO("new ClientRpcWrapper (rpc_thread_id = {})", rpc_->get_etid());
  for (int i = 0; i < shared_ctx.data_uri_list_.size(); i++) {
    AddDataServer(shared_ctx.data_uri_list_[i], i, shared_ctx.data_threads_[i]);
  }
  for (int i = 0; i < shared_ctx.meta_uri_list_.size(); i++) {
    AddMetaServer(shared_ctx.meta_uri_list_[i], shared_ctx.meta_threads_[i]);
  }
  WaitServerSessionConnected();
}

void ClientRpcWrapper::AddDataServer(std::string const &server_uri,
                                     int dataserver, int n_threads) {
  std::vector<int> data_sessions;
  for (int i = 0; i < n_threads; i++) {
    int session_num = rpc_->create_session(server_uri, i);
    erpc::rt_assert(session_num >= 0,
                    "Failed to create session to data server " + server_uri);
    data_sessions.push_back(session_num);
  }
  data_sessions_.push_back(data_sessions);
}

// Connect this client thread to all server threads
void ClientRpcWrapper::AddMetaServer(std::string const &server_uri,
                                     int n_threads) {
  std::vector<int> meta_sessions;
  for (int i = 1; i <= n_threads; i++) {
    int session_num = rpc_->create_session(server_uri, i);
    erpc::rt_assert(session_num >= 0,
                    "Failed to create session to meta server " + server_uri);
    meta_sessions.push_back(session_num);
  }
  meta_sessions_.push_back(meta_sessions);
}

auto ClientRpcWrapper::WaitServerSessionConnected() -> void {
  auto total_sessions_num = 0;
  for (auto &meta_sessions : meta_sessions_) {
    total_sessions_num += meta_sessions.size();
  }
  for (auto &data_session : data_sessions_) {
    total_sessions_num += data_session.size();
  }

  while (gConnectedSessionNum.load() != total_sessions_num) {
    rpc_->run_event_loop_once();
  }
  // SPDLOG_INFO("All server sessions connected");
}

auto ClientRpcWrapper::GetDataserverCount() -> int {
  return data_sessions_.size();
}

auto ClientRpcWrapper::GetDataserverThreadCount(int node_id) -> int {
  // yy TODO: a demo
  // FIXME: maybe we need to get this from server, rather than get it from local
  // here.
  return data_sessions_[node_id].size();
}

void ClientRpcWrapper::RpcHelloMetaServer(int thread_id) {
  // test meta server
  auto meta_sessions_num = 0;
  int thread_id_ = thread_id;
  int thread_meta = 0;
  for (auto &meta_sessions : meta_sessions_) {
    meta_sessions_num += meta_sessions.size();
    if (thread_id_ > meta_sessions.size()) {
      thread_id_ -= meta_sessions.size();
      thread_meta++;
    } else {
      break;
    }
  }
  if (thread_meta >= meta_sessions_.size()) {
    SPDLOG_ERROR("node_id_ {} is out of range", thread_id);
    return;
  }
  auto req = rpc_->alloc_msg_buffer_or_die(16);
  auto resp = rpc_->alloc_msg_buffer_or_die(16);
  memcpy(req.buf_, "hello to server", 15);
  ContContextT cc = {0, reinterpret_cast<char *>(resp.buf_)};
  rpc_->enqueue_request(meta_sessions_[thread_meta][thread_id_],
                        RPCType::kHelloReq, &req, &resp, ContFunc, &cc);
  while (cc.finished_ == 0) {
    rpc_->run_event_loop_once();
  }
  rpc_->free_msg_buffer(req);
  rpc_->free_msg_buffer(resp);
}

void ClientRpcWrapper::RpcHelloDataServer(int node_id, int thread_id) {
  // test data server
  if (node_id >= data_sessions_.size()) {
    SPDLOG_ERROR("node_id_ {} is out of range", node_id);
    return;
  }
  if (thread_id >= data_sessions_[node_id].size()) {
    SPDLOG_ERROR("thread_id_ {} is out of range", thread_id);
    return;
  }
  auto req = rpc_->alloc_msg_buffer_or_die(16);
  auto resp = rpc_->alloc_msg_buffer_or_die(16);
  memcpy(req.buf_, "hello to server", 15);
  ContContextT cc = {0, reinterpret_cast<char *>(resp.buf_)};
  rpc_->enqueue_request(data_sessions_[node_id][thread_id], RPCType::kHelloReq,
                        &req, &resp, ContFunc, &cc);
  while (cc.finished_ == 0) {
    rpc_->run_event_loop_once();
  }
  rpc_->free_msg_buffer(req);
  rpc_->free_msg_buffer(resp);
}

auto ClientRpcWrapper::TestCommunication() -> void {
  SPDLOG_INFO("Test Communication between meta servers");
  for (auto &meta_session : meta_sessions_) {
    for (int session : meta_session) {
      auto req = rpc_->alloc_msg_buffer_or_die(5);
      auto resp = rpc_->alloc_msg_buffer_or_die(1);
      memcpy(req.buf_, "Test", 4);
      ContContextT cc = {0, reinterpret_cast<char *>(resp.buf_)};
      rpc_->enqueue_request(session, RPCType::kTestMetaCommunicationReq, &req,
                            &resp, ContFunc, &cc);

      while (cc.finished_ == 0) {
        rpc_->run_event_loop_once();
      }
      rpc_->free_msg_buffer(req);
      rpc_->free_msg_buffer(resp);
    }
  }
}

auto ClientRpcWrapper::RpcPwrite(int node_id, uint64_t objuuid, uint64_t offset,
                                 uint64_t io_size,
                                 const void *io_buffer) -> int {
  DTRACE_PROBE4(DFS_CLIENT, RpcPwrite_enter, node_id, objuuid, offset, io_size);
  ssize_t ret = 0;
  if (node_id >= data_sessions_.size()) {
    SPDLOG_ERROR("node_id_ {} is out of range", node_id);
    return -1;
  }
  dfs::data::RequestType io_type = dfs::data::RequestType::RequestType_Write;
  // build io request
  flatbuffers::FlatBufferBuilder builder(1024);
  auto data_request = dfs::data::CreateDataRequest(
      builder, io_type, objuuid, offset, io_size,
      builder.CreateVector<uint8_t>(
          reinterpret_cast<const uint8_t *>(io_buffer), io_size));
  builder.Finish(data_request);
  auto data_request_buf = builder.GetBufferPointer();
  int data_request_size = builder.GetSize();

  auto req = rpc_->alloc_msg_buffer_or_die(data_request_size);
  auto resp = rpc_->alloc_msg_buffer_or_die(16);
  memcpy(req.buf_, data_request_buf, data_request_size);
  ContContextT cc = {0, reinterpret_cast<char *>(resp.buf_)};
  rpc_->enqueue_request(FastRandSessionNum(data_sessions_[node_id]),
                        RPCType::kDataReq, &req, &resp, ContFunc, &cc);
  SPDLOG_TRACE("Sent request");

  DTRACE_PROBE(DFS_CLIENT, RpcPwrite_wait_start);
  while (cc.finished_ == 0) {
    rpc_->run_event_loop_once();
  }
  DTRACE_PROBE(DFS_CLIENT, RpcPwrite_wait_end);

  ret = *reinterpret_cast<ssize_t *>(resp.buf_);

  rpc_->free_msg_buffer(req);
  rpc_->free_msg_buffer(resp);
  DTRACE_PROBE1(DFS_CLIENT, RpcPwrite_ret, ret);
  return ret;
}

auto ClientRpcWrapper::RpcPread(int node_id, uint64_t objuuid, uint64_t offset,
                                uint64_t io_size, void *io_buffer) -> int {

  DTRACE_PROBE4(DFS_CLIENT, RpcPread_enter, node_id, objuuid, offset, io_size);
  ssize_t ret = 0;
  if (node_id >= data_sessions_.size()) {
    SPDLOG_ERROR("node_id {} is out of range", node_id);
    return -1;
  }
  dfs::data::RequestType io_type = dfs::data::RequestType::RequestType_Read;

  // build io request
  flatbuffers::FlatBufferBuilder builder(1024);
  auto data_request = dfs::data::CreateDataRequest(
      builder, io_type, objuuid, offset, io_size,
      builder.CreateVector<uint8_t>(reinterpret_cast<uint8_t *>(io_buffer), 0));
  builder.Finish(data_request);
  auto data_request_buf = builder.GetBufferPointer();
  int data_request_size = builder.GetSize();

  auto req = rpc_->alloc_msg_buffer_or_die(data_request_size);
  auto resp = rpc_->alloc_msg_buffer_or_die(sizeof(ssize_t) + io_size);
  memcpy(req.buf_, data_request_buf, data_request_size);
  ContContextT cc = {0, reinterpret_cast<char *>(resp.buf_)};
  rpc_->enqueue_request(FastRandSessionNum(data_sessions_[node_id]),
                        RPCType::kDataReq, &req, &resp, ContFunc, &cc);
  SPDLOG_TRACE("Sent request");
  DTRACE_PROBE(DFS_CLIENT, RpcPread_wait_start);
  while (cc.finished_ == 0) {
    rpc_->run_event_loop_once();
  }
  DTRACE_PROBE(DFS_CLIENT, RpcPread_wait_end);

  ret = *reinterpret_cast<ssize_t *>(resp.buf_);
  if (ret != -1) { // not Error
    memcpy(io_buffer, resp.buf_ + sizeof(ssize_t), ret);
  }

  rpc_->free_msg_buffer(req);
  rpc_->free_msg_buffer(resp);
  DTRACE_PROBE1(DFS_CLIENT, RpcPread_ret, ret);
  return ret;
}

ClientRpcWrapper::~ClientRpcWrapper() {
  for (const auto &server : data_sessions_) {
    for (int session_num : server) {
      rpc_->destroy_session(session_num);
    }
  }
  for (const auto &server : meta_sessions_) {
    for (int session_num : server) {
      rpc_->destroy_session(session_num);
    }
  }

  using clock = std::chrono::steady_clock;
  constexpr auto kDisconnectTimeout = std::chrono::seconds(5);
  const auto deadline = clock::now() + kDisconnectTimeout;
  while (rpc_->num_active_sessions() > 0) {
    if (clock::now() > deadline) {
      SPDLOG_WARN(
          "ClientRpcWrapper destructor: {} sessions still active after "
          "{}s timeout; server-side ring entries may leak.",
          rpc_->num_active_sessions(),
          std::chrono::duration_cast<std::chrono::seconds>(kDisconnectTimeout)
              .count());
      break;
    }
    rpc_->run_event_loop_once();
  }

  rpc_.reset();
}

auto ClientRpcWrapper::MetaCall(flatbuffers::FlatBufferBuilder &fbb,
                                int req_type, int meta_num,
                                int resp_size) -> erpc::MsgBuffer {
  DTRACE_PROBE(DFS_CLIENT, MetaCall_enter);
  int req_size = fbb.GetSize();
  auto req = rpc_->alloc_msg_buffer_or_die(req_size);
  auto resp = rpc_->alloc_msg_buffer_or_die(resp_size);
  memcpy(req.buf_, fbb.GetBufferPointer(), req_size);

  ContContextT cc = {0, reinterpret_cast<char *>(resp.buf_)};
  SPDLOG_TRACE("Sent request");
  // Choose a random session from all established sessions.
  rpc_->enqueue_request(FastRandSessionNum(meta_sessions_[meta_num]), req_type,
                        &req, &resp, ContFunc, &cc);
  while (cc.finished_ == 0) {
    rpc_->run_event_loop_once();
  }
  rpc_->free_msg_buffer(req);
  DTRACE_PROBE(DFS_CLIENT, MetaCall_ret);
  return resp;
};

auto ClientRpcWrapper::PutInodeRequest(uint64_t inode_id,
                                       const dfs::Inode *i_buf) -> int {
  using mdrequest::CreateMDRequest;
  using mdrequest::MDOpType;
  // Phase 1: Build request
  flatbuffers::FlatBufferBuilder builder(128);
  auto vec_i_buf = builder.CreateVector<uint8_t>(
      reinterpret_cast<const uint8_t *>(i_buf), sizeof(dfs::Inode));
  auto md_req = CreateMDRequest(builder, uid_, gid_, MDOpType::MDOpType_Put,
                                inode_id, LoadLastSeenHLC(), vec_i_buf);
  builder.Finish(md_req);

  int meta_num = static_cast<int>(inode_id / (1ULL << INODE_ID_RANGE));
  auto resp = MetaCall(builder, RPCType::kMetaGeneralReq, meta_num,
                       sizeof(uint64_t) + 1);
  ObserveServerHLC(*reinterpret_cast<const uint64_t *>(resp.buf_));
  if (static_cast<char>(resp.buf_[sizeof(uint64_t)]) == -1) { // Error
    return -1;
  }
  rpc_->free_msg_buffer(resp);
  return 0;
}

auto ClientRpcWrapper::GetInodeRequest(uint64_t inode_id,
                                       dfs::Inode *i_buf) -> int {
  using mdrequest::CreateMDRequest;
  using mdrequest::MDOpType;
  // Phase 1: Build request
  flatbuffers::FlatBufferBuilder builder(128);
  auto md_req = CreateMDRequest(builder, uid_, gid_, MDOpType::MDOpType_Get,
                                inode_id, LoadLastSeenHLC());
  builder.Finish(md_req);

  int meta_num = static_cast<int>(inode_id / (1ULL << INODE_ID_RANGE));
  auto resp = MetaCall(builder, RPCType::kMetaGeneralReq, meta_num,
                       sizeof(uint64_t) + 1 + sizeof(dfs::Inode));
  ObserveServerHLC(*reinterpret_cast<const uint64_t *>(resp.buf_));
  if (static_cast<char>(resp.buf_[sizeof(uint64_t)]) == -1) { // Error
    return -1;
  }
  if (i_buf != nullptr) {
    *i_buf = *reinterpret_cast<dfs::Inode *>(resp.buf_ + sizeof(uint64_t) + 1);
  }
  rpc_->free_msg_buffer(resp);
  return 0;
}

auto ClientRpcWrapper::PathCommonRequest(const char *path,
                                         mdrequest::MDOpType op, uint32_t mode,
                                         void *buf) -> int {
  using mdrequest::CreateMDPathCommonRequest;
  using mdrequest::GetMDPathCommonResponse;
  using mdrequest::MDOpType;
  SPDLOG_TRACE("{}_request for {}", EnumNameMDOpType(op), path);
  DTRACE_PROBE3(DFS_CLIENT, PathCommonRequest_enter, path, op, mode);

  // if (op == MDOpType_Rename || op == MDOpType_Link) {
  //   std::cout << "PathCommonRequest: " << EnumNameMDOpType(op) << " " << path
  //             << " " << static_cast<const char *>(buf) << std::endl;
  // } else {
  //   std::cout << "PathCommonRequest: " << EnumNameMDOpType(op) << " " << path
  //             << std::endl;
  // }

  bool buf_is_for_return =
      (op == MDOpType_Stat || op == MDOpType_Create || op == MDOpType_OpenDir);
  // For stat, buf is output buffer of type <inode*>. For other, it's new
  // path.
  bool omit_new_path = buf_is_for_return || buf == nullptr;

  // Phase 1: Find cache
  int meta_num = 0;
  meta_num = FastRandSessionNum(meta_sessions_.size());

  flatbuffers::FlatBufferBuilder builder(128);
  auto md_req = CreateMDPathCommonRequest(
      builder, uid_, gid_, op, builder.CreateString(path), mode,
      omit_new_path ? 0 : builder.CreateString(static_cast<const char *>(buf)),
      meta_num, LoadLastSeenHLC());
  builder.Finish(md_req);
  int resp_size = 300;
  if (op == MDOpType_OpenDir) {
    resp_size += sizeof(DirHandle);
  }
  // Retry on -EAGAIN (server WAL is full / under backpressure).
  constexpr int kEagainBaseUs = 100;
  constexpr int kEagainMaxBackoffUs = 5000;
  erpc::MsgBuffer msg_buffer;
  const mdrequest::MDPathCommonResponse *resp = nullptr;
  for (int attempt = 0;; ++attempt) {
    msg_buffer =
        MetaCall(builder, RPCType::kMetaPathCommonReq, meta_num, resp_size);
    resp = GetMDPathCommonResponse(msg_buffer.buf_);
    ObserveServerHLC(resp->server_hlc());
    if (resp->flag() != -EAGAIN) {
      break;
    }
    rpc_->free_msg_buffer(msg_buffer);
    int backoff_us =
        std::min(kEagainBaseUs * (attempt + 1), kEagainMaxBackoffUs);
    std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
  }

  if (resp->flag() != 0) {
    SPDLOG_TRACE("{}_request for {} failed", EnumNameMDOpType(op), path);
  }
  if (resp->flag() == -1) { // Error
    rpc_->free_msg_buffer(msg_buffer);
    return -1;
  }

  if (buf_is_for_return && buf != nullptr) {
    if (op != MDOpType_OpenDir) {
      memcpy(buf, msg_buffer.buf_ + resp->length(), sizeof(Inode));
    } else {
      memcpy(buf, msg_buffer.buf_ + resp->length(), sizeof(DirHandle));
    }
  }
  rpc_->free_msg_buffer(msg_buffer);

  DTRACE_PROBE(DFS_CLIENT, PathCommonRequest_ret);
  return 0;
}

auto ClientRpcWrapper::FDCommonRequest(uint64_t id, mdrequest::MDOpType op,
                                       off_t offset, uint32_t u32arg, void *buf,
                                       int meta_num, uint64_t u64arg) -> int {
  using mdrequest::CreateMDFDCommonRequest;
  using mdrequest::EnumNameMDOpType;
  using mdrequest::MDOpType;

  SPDLOG_TRACE("FD_common_request called of type {}", EnumNameMDOpType(op));
  if (op != mdrequest::MDOpType_GetDents &&
      op != mdrequest::MDOpType_GetDentViews) {
    SPDLOG_ERROR(
        "Op type is {}, but only getdents({}) and getdentviews({}) are "
        "implemented.",
        EnumNameMDOpType(op), EnumNameMDOpType(mdrequest::MDOpType_GetDents),
        EnumNameMDOpType(mdrequest::MDOpType_GetDentViews));
  }
  // Phase 1: Build request
  flatbuffers::FlatBufferBuilder builder(128);
  auto md_req = CreateMDFDCommonRequest(builder, uid_, gid_, op, id, offset,
                                        u32arg, u64arg, 0, LoadLastSeenHLC());
  builder.Finish(md_req);
  if (meta_num == -1) {
    meta_num = static_cast<int>(id / (1ULL << INODE_ID_RANGE));
  }
  auto resp = MetaCall(builder, RPCType::kMetaFDCommonReq, meta_num,
                       sizeof(uint64_t) + u32arg + 1 + sizeof(uint32_t));
  SPDLOG_TRACE(
      "FD_common_request got {} bytes from meta {}, rpc_thread_id = {}, id = "
      "{}",
      resp.get_data_size(), meta_num, rpc_->get_etid(), id);
  ObserveServerHLC(*reinterpret_cast<const uint64_t *>(resp.buf_));

  if (static_cast<char>(resp.buf_[sizeof(uint64_t)]) == -1) { // Error
    rpc_->free_msg_buffer(resp);
    return static_cast<int>(op == mdrequest::MDOpType_GetDents) - 1;
  }
  uint32_t read_size =
      *reinterpret_cast<uint32_t *>(resp.buf_ + sizeof(uint64_t) + 1);
  memcpy(buf, resp.buf_ + sizeof(uint64_t) + 1 + sizeof(uint32_t), read_size);
  rpc_->free_msg_buffer(resp);
  return (op == mdrequest::MDOpType_GetDents ||
          op == mdrequest::MDOpType_GetDentViews)
             ? read_size
             : 0;
}

auto ClientRpcWrapper::BatchGetDentViews(
    uint64_t id, uint64_t read_cutoff_version, int n, const int *meta_nums,
    const off_t *offsets, char *const *out_bufs, uint32_t chunk_size,
    int *out_sizes) -> int {
  using mdrequest::CreateMDFDCommonRequest;
  using mdrequest::MDOpType_GetDentViews;

  if (n <= 0) {
    return 0;
  }

  // Reserve per-call state up front; FlatBufferBuilder is move-only, so build
  // the builders with emplace_back rather than default-constructing a vector.
  std::vector<flatbuffers::FlatBufferBuilder> builders;
  builders.reserve(n);
  for (int i = 0; i < n; ++i) {
    builders.emplace_back(128);
  }
  std::vector<erpc::MsgBuffer> req_bufs(n);
  std::vector<erpc::MsgBuffer> resp_bufs(n);
  std::vector<ContContextT> ccs(n);

  const uint64_t hlc = LoadLastSeenHLC();
  const int resp_size =
      static_cast<int>(sizeof(uint64_t) + chunk_size + 1 + sizeof(uint32_t));

  // Phase 1 — build each flatbuffer, allocate req+resp MsgBuffers, enqueue.
  // All requests go out back-to-back without blocking so eRPC can pipeline
  // them on the wire.
  for (int i = 0; i < n; ++i) {
    auto &b = builders[i];
    auto md_req = CreateMDFDCommonRequest(b, uid_, gid_, MDOpType_GetDentViews,
                                          id, offsets[i], chunk_size,
                                          read_cutoff_version, 0, hlc);
    b.Finish(md_req);

    int req_size = b.GetSize();
    req_bufs[i] = rpc_->alloc_msg_buffer_or_die(req_size);
    resp_bufs[i] = rpc_->alloc_msg_buffer_or_die(resp_size);
    memcpy(req_bufs[i].buf_, b.GetBufferPointer(), req_size);

    ccs[i].finished_ = 0;
    ccs[i].buf_ = reinterpret_cast<char *>(resp_bufs[i].buf_);

    rpc_->enqueue_request(FastRandSessionNum(meta_sessions_[meta_nums[i]]),
                          RPCType::kMetaFDCommonReq, &req_bufs[i],
                          &resp_bufs[i], ContFunc, &ccs[i]);
  }

  // Phase 2 — drain the event loop until every continuation has fired. The
  // event loop is single-threaded (this client owns the Rpc), so a simple
  // unsynchronized scan over ccs[] is sufficient.
  while (true) {
    rpc_->run_event_loop_once();
    bool any_pending = false;
    for (int i = 0; i < n; ++i) {
      if (ccs[i].finished_ == 0) {
        any_pending = true;
        break;
      }
    }
    if (!any_pending) {
      break;
    }
  }

  // Phase 3 — decode headers, memcpy payloads out, free MsgBuffers.
  int overall_ret = 0;
  for (int i = 0; i < n; ++i) {
    ObserveServerHLC(*reinterpret_cast<const uint64_t *>(resp_bufs[i].buf_));
    if (static_cast<char>(resp_bufs[i].buf_[sizeof(uint64_t)]) == -1) {
      out_sizes[i] = -1;
      overall_ret = -1;
    } else {
      uint32_t read_size = *reinterpret_cast<uint32_t *>(resp_bufs[i].buf_ +
                                                         sizeof(uint64_t) + 1);
      memcpy(out_bufs[i],
             resp_bufs[i].buf_ + sizeof(uint64_t) + 1 + sizeof(uint32_t),
             read_size);
      out_sizes[i] = static_cast<int>(read_size);
    }
    rpc_->free_msg_buffer(req_bufs[i]);
    rpc_->free_msg_buffer(resp_bufs[i]);
  }
  return overall_ret;
}

} // namespace dfs
