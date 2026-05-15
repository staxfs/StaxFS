#include "data_generated.h"
#include "flatbuffers/flatbuffer_builder.h"
#include "nexus.h"
#include <cstdint>
#include <rpc.h>
#include <spdlog/spdlog.h>

static constexpr uint8_t kReqType = 2;
static constexpr size_t kMsgSize = 16;
erpc::Rpc<erpc::CTransport> *gRpc;
erpc::MsgBuffer gReq;
erpc::MsgBuffer gResp;
using std::string;

void ContFunc(void * /*unused*/, void * /*unused*/) {
  SPDLOG_INFO("{}", (char *)gResp.buf_);
}

void SmHandler(int /*unused*/, erpc::SmEventType /*unused*/,
               erpc::SmErrType /*unused*/, void * /*unused*/) {
  SPDLOG_INFO("Session up");
}

auto main() -> int {
  std::string client_uri = "88.10.10.62:31850";
  erpc::Nexus nexus(client_uri);

  gRpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 0, SmHandler);

  std::string server_uri = "88.10.10.44:31850";
  int session_num = gRpc->create_session(server_uri, 0);

  while (!gRpc->is_connected(session_num)) {
    gRpc->run_event_loop_once();
  }

  SPDLOG_INFO("Connected to server");
  gReq = gRpc->alloc_msg_buffer_or_die(kMsgSize);
  gResp = gRpc->alloc_msg_buffer_or_die(kMsgSize);

  memcpy(gReq.buf_, "hello to server", 15);

  gRpc->enqueue_request(session_num, kReqType, &gReq, &gResp, ContFunc,
                        nullptr);
  SPDLOG_INFO("Sent request");
  gRpc->run_event_loop(10);

  {
    dfs::data::RequestType io_type = dfs::data::RequestType::RequestType_Write;
    uint64_t objuuid = 1351;
    uint64_t offset = 123;
    uint64_t io_size = 15;
    char *io_buffer = new char[1024];
    memcpy(io_buffer, "hello to server", io_size);

    // build io request
    flatbuffers::FlatBufferBuilder builder(1024);
    auto data_request = dfs::data::CreateDataRequest(
        builder, io_type, objuuid, offset, io_size,
        builder.CreateVector<uint8_t>(reinterpret_cast<uint8_t *>(io_buffer),
                                      io_size));
    builder.Finish(data_request);
    auto data_request_buf = builder.GetBufferPointer();
    int data_request_size = builder.GetSize();

    gReq = gRpc->alloc_msg_buffer_or_die(data_request_size);
    gResp = gRpc->alloc_msg_buffer_or_die(16);
    memcpy(gReq.buf_, data_request_buf, data_request_size);

    gRpc->enqueue_request(session_num, 66, &gReq, &gResp, ContFunc, nullptr);
    gRpc->run_event_loop(10);

    delete[] io_buffer;
  }

  {
    dfs::data::RequestType io_type = dfs::data::RequestType::RequestType_Read;
    uint64_t objuuid = 1351;
    uint64_t offset = 123;
    uint64_t io_size = 15;
    char *io_buffer = new char[1024];
    // memcpy(io_buffer, "hello to server", io_size);

    // build io request
    flatbuffers::FlatBufferBuilder builder(1024);
    auto data_request = dfs::data::CreateDataRequest(
        builder, io_type, objuuid, offset, io_size,
        builder.CreateVector<uint8_t>(reinterpret_cast<uint8_t *>(io_buffer),
                                      0));
    builder.Finish(data_request);
    auto data_request_buf = builder.GetBufferPointer();
    int data_request_size = builder.GetSize();

    gReq = gRpc->alloc_msg_buffer_or_die(data_request_size);
    gResp = gRpc->alloc_msg_buffer_or_die(1 + io_size);
    memcpy(gReq.buf_, data_request_buf, data_request_size);
    SPDLOG_INFO("Sent request");

    gRpc->enqueue_request(session_num, 66, &gReq, &gResp, ContFunc, nullptr);
    gRpc->run_event_loop(10);

    delete[] io_buffer;
  }

  gRpc->run_event_loop(100000);
  delete gRpc;
  return 0;
}
