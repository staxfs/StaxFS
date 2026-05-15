#pragma once

#include "flatbuffers/flatbuffer_builder.h"
#include "msg_buffer.h"
#include "server/metadata.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <rpc.h>
#include <string>
#include <sys/sdt.h>
#include <utility>
#include <vector>

namespace dfs {
class Metadata;

struct ServerContext {
  erpc::Rpc<erpc::CTransport> *rpc_ = nullptr;
  size_t rpc_id_;
  std::vector<int64_t> nr_reqs_;
  int meta_num_;
  std::vector<std::string> meta_uri_list_;
  std::vector<int> meta_rpc_num_;
  std::vector<int> meta_to_meta_sessions_;
};

struct GuardianContext {
  erpc::Rpc<erpc::CTransport> *rpc_ = nullptr;
  size_t rpc_id_;
  int meta_num_;
  std::vector<std::string> meta_uri_list_;
  std::vector<int> meta_rpc_num_;
  std::vector<std::vector<int>> meta_to_meta_sessions_;
  std::vector<int64_t> reqs_;
  std::atomic<uint32_t> heart_num_{0};
};

struct ContContext {
  int finished_ = 0;
};

void ContFunction(void *context, void *tag);

auto GuardianToGuardianMetaCall(flatbuffers::FlatBufferBuilder &fbb,
                                int req_type, int meta_num,
                                GuardianContext &guardian_context,
                                int resp_size = 1) -> erpc::MsgBuffer;
auto FastRandSessionNum(const std::vector<int> &sn_vec) -> int;

auto GuardianToServerMetaCall(flatbuffers::FlatBufferBuilder &fbb, int req_type,
                              int meta_num, GuardianContext &guardian_context,
                              int resp_size = 1) -> erpc::MsgBuffer;

auto GuardianToGuardianMetaCallSplit(flatbuffers::FlatBufferBuilder &fbb,
                                     int req_type, int meta_num,
                                     GuardianContext &guardian_context,
                                     int resp_size = 1) -> erpc::MsgBuffer;

auto GuardianToServerMetaCallSplit(flatbuffers::FlatBufferBuilder &fbb,
                                   int req_type, int meta_num,
                                   GuardianContext &guardian_context,
                                   int resp_size = 1) -> erpc::MsgBuffer;

auto ServerMetaCall(flatbuffers::FlatBufferBuilder &fbb, int req_type,
                    int meta_num, ServerContext &server_context,
                    int resp_size = 300) -> erpc::MsgBuffer;

auto Heartbeat(GuardianContext &guardian_context) -> struct timespec;

void ServicePendingRemoteInodeChanges(GuardianContext &guardian_context);

} // namespace dfs
