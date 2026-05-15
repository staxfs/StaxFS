#include "server/rpc_server.h"

#include "common/dfs.h"
#include "mdguardiancommonrequest_generated.h"
#include "mdguardianheartrequest_generated.h"
#include "mdpathcommonrequest_generated.h"
#include "mdpathcommonresponse_generated.h"
#include "mdpersistencerequest_generated.h"
#include "mdrequest_generated.h"
#include "msg_buffer.h"
#include "server/cxl_persistence.h"
#include "spdlog/spdlog.h"
#include <cstdint>
#include <mutex>
#include <vector>

int gHeartbeatCycle = 10; // (s)
struct timespec gStartTime, gEndTime;
std::atomic<bool> gLoadBalance = false;
extern std::unique_ptr<dfs::Metadata> gMetaManager;
erpc::FastRand gFastrand;

namespace dfs {

namespace {

auto SendRemoteInodeChangesOnce(
    GuardianContext &guardian_context, int target_meta,
    const std::vector<RemoteInodeChange> &changes) -> size_t {
  if (changes.empty()) {
    return 0;
  }

  using mdrequest::CreateMDPersistenceRequest;

  std::vector<uint8_t> change_op;
  std::vector<uint64_t> change_id;
  std::vector<int64_t> change_value;
  std::vector<uint64_t> change_version;

  for (const auto &change : changes) {
    change_op.push_back(static_cast<uint8_t>(change.op_));
    change_id.push_back(change.inode_id_);
    change_value.push_back(change.value_);
    change_version.push_back(change.version_);
  }

  flatbuffers::FlatBufferBuilder builder;
  auto req = CreateMDPersistenceRequest(
      builder, builder.CreateVector(change_op), builder.CreateVector(change_id),
      builder.CreateVector(change_value), builder.CreateVector(change_version));
  builder.Finish(req);

  const int rpc_max_size = erpc::Rpc<erpc::CTransport>::get_max_msg_size();
  erpc::MsgBuffer resp;
  if (static_cast<int>(builder.GetSize()) <= rpc_max_size) {
    resp = GuardianToServerMetaCall(builder, RPCType::kMetaPersistenceReq,
                                    target_meta, guardian_context,
                                    sizeof(uint32_t));
  } else {
    resp = GuardianToServerMetaCallSplit(
        builder, RPCType::kMetaPersistenceReqSplit, target_meta,
        guardian_context, sizeof(uint32_t));
  }
  uint32_t inserted = 0;
  std::memcpy(&inserted, resp.buf_, sizeof(uint32_t));
  guardian_context.rpc_->free_msg_buffer(resp);

  if (inserted == 0xFFFFFFFFU) {
    SPDLOG_ERROR("Remote inode forwarding fatal: sender meta {} -> target {} "
                 "(malformed split chunk); will retry batch of {}",
                 guardian_context.meta_num_, target_meta, changes.size());
    return 0;
  }
  if (inserted > changes.size()) {
    SPDLOG_ERROR("Remote inode forwarding got count {} > sent {} from target "
                 "{}; clamping to 0 and retrying",
                 inserted, changes.size(), target_meta);
    return 0;
  }
  return inserted;
}

} // namespace

void ContFunction(void *context, void *tag) {
  auto *ret = reinterpret_cast<ContContext *>(tag);
  SPDLOG_TRACE("Receive response");
  ret->finished_ = 1;
}

auto GuardianToGuardianMetaCall(flatbuffers::FlatBufferBuilder &fbb,
                                int req_type, int meta_num,
                                GuardianContext &guardian_context,
                                int resp_size) -> erpc::MsgBuffer {
  DTRACE_PROBE(DFS_SERVER, GuardianToGuardianMetaCall_enter);
  int req_size = fbb.GetSize();
  int rpc_max_size = erpc::Rpc<erpc::CTransport>::get_max_msg_size();
  if (fbb.GetSize() > rpc_max_size) {
    SPDLOG_ERROR("too big {} > {}, need use GuardianToGuardianMetaCallSplit()",
                 req_size, rpc_max_size);
  }

  auto req = guardian_context.rpc_->alloc_msg_buffer_or_die(req_size);
  auto resp = guardian_context.rpc_->alloc_msg_buffer_or_die(resp_size);
  memcpy(req.buf_, fbb.GetBufferPointer(), req_size);

  ContContext cc = {0};
  guardian_context.rpc_->enqueue_request(
      guardian_context.meta_to_meta_sessions_[meta_num][0], req_type, &req,
      &resp, ContFunction, &cc);
  while (cc.finished_ == 0) {
    guardian_context.rpc_->run_event_loop_once();
  }
  guardian_context.rpc_->free_msg_buffer(req);
  DTRACE_PROBE(DFS_SERVER, GuardianToGuardianMetaCall_ret);
  return resp;
};

auto FastRandSessionNum(const std::vector<int> &sn_vec) -> int {
  uint32_t x = gFastrand.next_u32();
  size_t rand_index = (static_cast<size_t>(x) * (sn_vec.size() - 1)) >> 32;
  return sn_vec[rand_index + 1];
}

auto GuardianToServerMetaCall(flatbuffers::FlatBufferBuilder &fbb, int req_type,
                              int meta_num, GuardianContext &guardian_context,
                              int resp_size) -> erpc::MsgBuffer {
  DTRACE_PROBE(DFS_SERVER, GuardianToServerMetaCall_enter);
  int req_size = fbb.GetSize();
  int rpc_max_size = erpc::Rpc<erpc::CTransport>::get_max_msg_size();
  if (fbb.GetSize() > rpc_max_size) {
    SPDLOG_ERROR("too big {} > {}, need use GuardianToServerMetaCallSplit()",
                 req_size, rpc_max_size);
  }

  auto req = guardian_context.rpc_->alloc_msg_buffer_or_die(req_size);
  auto resp = guardian_context.rpc_->alloc_msg_buffer_or_die(resp_size);
  memcpy(req.buf_, fbb.GetBufferPointer(), req_size);

  ContContext cc = {0};
  guardian_context.rpc_->enqueue_request(
      FastRandSessionNum(guardian_context.meta_to_meta_sessions_[meta_num]),
      req_type, &req, &resp, ContFunction, &cc);
  while (cc.finished_ == 0) {
    guardian_context.rpc_->run_event_loop_once();
  }
  guardian_context.rpc_->free_msg_buffer(req);
  DTRACE_PROBE(DFS_SERVER, GuardianToServerMetaCall_ret);
  return resp;
}

auto GuardianToGuardianMetaCallSplit(flatbuffers::FlatBufferBuilder &fbb,
                                     int req_type, int meta_num,
                                     GuardianContext &guardian_context,
                                     int resp_size) -> erpc::MsgBuffer {
  DTRACE_PROBE(DFS_SERVER, GuardianToGuardianMetaCallSplit_enter);
  int fbb_size = fbb.GetSize();
  int rpc_max_size = erpc::Rpc<erpc::CTransport>::get_max_msg_size() - 5 - 1;
  int chunk_num = (fbb_size + rpc_max_size - 1) / rpc_max_size;

  erpc::MsgBuffer resp;
  for (int i = 0; i < chunk_num; i++) {
    int chunk_size = std::min(rpc_max_size, fbb_size - i * rpc_max_size);
    auto req = guardian_context.rpc_->alloc_msg_buffer_or_die(chunk_size + 5);
    resp = guardian_context.rpc_->alloc_msg_buffer_or_die(resp_size);
    req.buf_[0] = 0;
    memcpy(req.buf_ + 1, &(guardian_context.meta_num_), sizeof(int));
    if (i == chunk_num - 1) {
      req.buf_[0] = 1;
    }
    memcpy(req.buf_ + 5, fbb.GetBufferPointer() + i * rpc_max_size, chunk_size);

    ContContext cc = {0};
    guardian_context.rpc_->enqueue_request(
        guardian_context.meta_to_meta_sessions_[meta_num][0], req_type, &req,
        &resp, ContFunction, &cc);
    while (cc.finished_ == 0) {
      guardian_context.rpc_->run_event_loop_once();
    }
    guardian_context.rpc_->free_msg_buffer(req);
    if (resp.buf_[0] != 0) {
      return resp;
    }
    if (i != chunk_num - 1) {
      guardian_context.rpc_->free_msg_buffer(resp);
    }
  }

  DTRACE_PROBE(DFS_SERVER, GuardianToGuardianMetaCallSplit_ret);
  return resp;
};

auto GuardianToServerMetaCallSplit(flatbuffers::FlatBufferBuilder &fbb,
                                   int req_type, int meta_num,
                                   GuardianContext &guardian_context,
                                   int resp_size) -> erpc::MsgBuffer {
  DTRACE_PROBE(DFS_SERVER, GuardianToServerMetaCallSplit_enter);
  int fbb_size = fbb.GetSize();
  int rpc_max_size = erpc::Rpc<erpc::CTransport>::get_max_msg_size() - 5 - 1;
  int chunk_num = (fbb_size + rpc_max_size - 1) / rpc_max_size;

  // All chunks of a single logical message must land on the same receiver
  // RPC thread so the handler can reassemble them. Pick ONE random
  // "server-lane" session up front and reuse it across the whole split.
  int session_num =
      FastRandSessionNum(guardian_context.meta_to_meta_sessions_[meta_num]);

  erpc::MsgBuffer resp;
  for (int i = 0; i < chunk_num; i++) {
    int chunk_size = std::min(rpc_max_size, fbb_size - i * rpc_max_size);
    auto req = guardian_context.rpc_->alloc_msg_buffer_or_die(chunk_size + 5);
    resp = guardian_context.rpc_->alloc_msg_buffer_or_die(resp_size);
    req.buf_[0] = 0;
    memcpy(req.buf_ + 1, &(guardian_context.meta_num_), sizeof(int));
    if (i == chunk_num - 1) {
      req.buf_[0] = 1;
    }
    memcpy(req.buf_ + 5, fbb.GetBufferPointer() + i * rpc_max_size, chunk_size);

    ContContext cc = {0};
    guardian_context.rpc_->enqueue_request(session_num, req_type, &req, &resp,
                                           ContFunction, &cc);
    while (cc.finished_ == 0) {
      guardian_context.rpc_->run_event_loop_once();
    }
    guardian_context.rpc_->free_msg_buffer(req);
    // Intermediate chunks: receiver responds with a sentinel ack; only the
    // final chunk's response carries application-meaningful payload (e.g.
    // an inserted-count for kMetaPersistenceReq)
    if (i != chunk_num - 1) {
      guardian_context.rpc_->free_msg_buffer(resp);
    }
  }

  DTRACE_PROBE(DFS_SERVER, GuardianToServerMetaCallSplit_ret);
  return resp;
};

auto ServerMetaCall(flatbuffers::FlatBufferBuilder &fbb, int req_type,
                    int meta_num, ServerContext &server_context,
                    int resp_size) -> erpc::MsgBuffer {
  DTRACE_PROBE(DFS_SERVER, ServerMetaCall_enter);
  int req_size = fbb.GetSize();
  auto req = server_context.rpc_->alloc_msg_buffer_or_die(req_size);
  auto resp = server_context.rpc_->alloc_msg_buffer_or_die(resp_size);
  memcpy(req.buf_, fbb.GetBufferPointer(), req_size);

  ContContext cc = {0};
  server_context.rpc_->enqueue_request(
      server_context.meta_to_meta_sessions_[meta_num], req_type, &req, &resp,
      ContFunction, &cc);
  while (cc.finished_ == 0) {
    server_context.rpc_->run_event_loop_once();
  }
  server_context.rpc_->free_msg_buffer(req);
  DTRACE_PROBE(DFS_SERVER, ServerMetaCall_ret);
  return resp;
};

auto Heartbeat(GuardianContext &guardian_context) -> struct timespec {
  DTRACE_PROBE(DFS_SERVER, Heartbeat_enter);
  if (guardian_context.meta_num_ != 0) {
    using dfs::mdrequest::CreateMDGuardianCommonRequest;
    using dfs::mdrequest::GetMDGuardianHeartRequest;

    flatbuffers::FlatBufferBuilder builder;
    auto req = CreateMDGuardianCommonRequest(
        builder, guardian_context.meta_num_,
        builder.CreateVector(guardian_context.reqs_));
    builder.Finish(req);

    auto resp = GuardianToGuardianMetaCall(
        builder, dfs::RPCType::kGuardianCommonReq, 0, guardian_context, 50);
    if (resp.buf_[0] != 0) {
      SPDLOG_ERROR("heart request from guardian {} to guardian {} failed",
                   guardian_context.meta_num_, 0);
    }
    auto resp_time = GetMDGuardianHeartRequest(resp.buf_ + 1);
    struct timespec time;
    time.tv_sec = resp_time->tv_sec();
    time.tv_nsec = resp_time->tv_nsec();

    guardian_context.rpc_->free_msg_buffer(resp);
    guardian_context.reqs_[guardian_context.meta_num_] = 0;
    guardian_context.heart_num_.store(0);
    return time;
  }

  DTRACE_PROBE(DFS_SERVER, Heartbeat_ret);
  return {};

}

void ServicePendingRemoteInodeChanges(GuardianContext &guardian_context) {

  if (gCXLPersistence == nullptr) {
    return;
  }

  constexpr size_t kForwardBatchFloor = 512;
  constexpr size_t kForwardBatchCeil = 65536;
  constexpr size_t kForwardBatchInitial = 4096;

  std::vector<std::vector<RemoteInodeChange>> snapshots;
  {
    std::lock_guard<std::mutex> lk(gCXLPersistence->PendingForwardMu());
    auto &pending = gCXLPersistence->PendingForward();
    auto &caps = gCXLPersistence->ForwardBatchCap();
    snapshots.resize(pending.size());
    if (caps.size() < pending.size()) {
      caps.resize(pending.size(), kForwardBatchInitial);
    }
    for (size_t t = 0; t < pending.size(); ++t) {
      if (pending[t].empty()) {
        continue;
      }
      const size_t take = std::min(pending[t].size(), caps[t]);
      snapshots[t].assign(std::make_move_iterator(pending[t].begin()),
                          std::make_move_iterator(pending[t].begin() + take));
      pending[t].erase(pending[t].begin(), pending[t].begin() + take);
    }
  }

  for (size_t t = 0; t < snapshots.size(); ++t) {
    if (snapshots[t].empty() ||
        static_cast<int>(t) == guardian_context.meta_num_) {
      continue;
    }
    const size_t sent = snapshots[t].size();
    const size_t inserted = SendRemoteInodeChangesOnce(
        guardian_context, static_cast<int>(t), snapshots[t]);

    std::lock_guard<std::mutex> lk(gCXLPersistence->PendingForwardMu());
    auto &pending = gCXLPersistence->PendingForward();
    auto &caps = gCXLPersistence->ForwardBatchCap();
    if (caps.size() <= t) {
      caps.resize(t + 1, kForwardBatchInitial);
    }
    if (inserted >= sent) {
      caps[t] = std::min(caps[t] * 2, kForwardBatchCeil);
      continue;
    }
    caps[t] = std::max(inserted, kForwardBatchFloor);
    SPDLOG_INFO("ServicePendingRemoteInodeChanges: to meta {} sent={} "
                "inserted={} new_cap={}",
                static_cast<int>(t), sent, inserted, caps[t]);
    if (pending.size() <= t) {
      pending.resize(t + 1);
    }
    auto &dst = pending[t];
    dst.insert(dst.begin(),
               std::make_move_iterator(snapshots[t].begin() + inserted),
               std::make_move_iterator(snapshots[t].end()));
  }
}

} // namespace dfs
