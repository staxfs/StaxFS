#include "data_generated.h"
#include "flatbuffers/flatbuffer_builder.h"
#include "mdpathcommonrequest_generated.h"
#include "mdrequest_generated.h"
#include "metadata_generated.h"
#include "server/metadata.h"

// Stub for gMetaManager (defined in server_main.cc, pulled in by
// dfs-prototype-server-lib → rpc_server.cc)
std::unique_ptr<dfs::Metadata> gMetaManager = nullptr;

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/types.h>

TEST(SerdesTest, Smoke) {
  using namespace dfs::metadata;
  flatbuffers::FlatBufferBuilder builder(1024);
  uint64_t inode_id = 1;
  auto inode = CreateInode(builder, inode_id);
  builder.Finish(inode);
  auto buf = builder.GetBufferPointer();
  auto inode2 = GetInode(buf);
  ASSERT_EQ(inode2->id(), inode_id);
}

TEST(SerdesDataTest, Smoke) {
  using namespace dfs::data;
  flatbuffers::FlatBufferBuilder builder(1024);
  uint64_t objuuid = 1351;
  uint64_t offset = 123;
  std::string buffer_str = "hello world";
  uint64_t size = buffer_str.size();
  char *buffer = const_cast<char *>(buffer_str.c_str());

  auto data_request = CreateDataRequest(
      builder, dfs::data::RequestType::RequestType_Read, objuuid, offset, size,
      builder.CreateVector<uint8_t>(reinterpret_cast<uint8_t *>(buffer), size));
  builder.Finish(data_request);
  auto buf = builder.GetBufferPointer();
  int datarequest_size = builder.GetSize();
  spdlog::info("datarequest_size: {}", datarequest_size);

  {
    char *buf2 = new char[datarequest_size];
    memcpy(buf2, buf, datarequest_size);
    auto data_request2 = GetDataRequest(buf2);
    ASSERT_EQ(data_request2->io_type(), 1);
    ASSERT_EQ(data_request2->objuuid(), objuuid);
    ASSERT_EQ(data_request2->offset(), offset);
    ASSERT_EQ(data_request2->size(), size);
    ASSERT_TRUE(memcmp(buffer, data_request2->buffer()->data(), size) == 0);
    // ASSERT_EQ(data_request2->buffer()->str(), buffer);
  }
}

TEST(SerdesMetaTest, MDPathCommonRequest) {
  using namespace dfs::mdrequest;
  uint32_t uid = 1;
  uint32_t gid = 2;
  uint64_t last_seen_hlc = 0x12345678ULL;
  const char *path = "/tmp/dfs/serdes/path";
  // Use a buffer and a size to represent a request message.
  uint8_t *buf_send;
  int request_size;
  char buf_recv[1024];
  // ########### Test 3: rename(path, newpath) -> 0 | -1 ################
  { // Phase 1: Build and send request
    flatbuffers::FlatBufferBuilder builder(128);
    const char *newpath = "/tmp/dfs/serdes/newpath";
    auto req_type = MDOpType::MDOpType_Rename;
    auto rename_req = CreateMDPathCommonRequest(
        builder, uid, gid, req_type, builder.CreateString(path), 0,
        builder.CreateString(newpath), 0, last_seen_hlc);
    builder.Finish(rename_req);
    buf_send = builder.GetBufferPointer();
    request_size = builder.GetSize();
    memcpy(buf_recv, buf_send, request_size);
    spdlog::info("rename request size = {}", request_size);
  }
  { // Phase 2: Unpack request and check
    auto req_recv = GetMDPathCommonRequest(buf_recv);
    EXPECT_EQ(req_recv->uid(), 1);
    EXPECT_EQ(req_recv->gid(), 2);
    EXPECT_EQ(req_recv->op(), MDOpType_Rename);
    EXPECT_STREQ(req_recv->path()->c_str(), path);
    EXPECT_EQ(req_recv->mode(), 0);
    EXPECT_STREQ(req_recv->newpath()->c_str(), "/tmp/dfs/serdes/newpath");
    EXPECT_EQ(req_recv->last_seen_hlc(), last_seen_hlc);
    spdlog::info("recv rename {} -> {} ok", req_recv->path()->c_str(),
                 req_recv->newpath()->c_str());
  }
}

TEST(SerdesMetaTest, MDGeneralRequest) {
  uint32_t uid = 1;
  uint32_t gid = 2;
  uint64_t inode_id = 0x87654321;
  uint64_t last_seen_hlc = 0x112233445566ULL;
  dfs::Inode inode(inode_id, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  // Use a buffer and a size to represent a request message.
  uint8_t *buf_send;
  int request_size;
  char buf_recv[1024];
  // ########### Test 2: put(inode_id, inode_ptr) -> 0 | -1 ################
  { // Phase 1: Build and send request
    flatbuffers::FlatBufferBuilder builder(256);
    auto vec = builder.CreateVector<uint8_t>(
        reinterpret_cast<const u_char *>(&inode), sizeof(dfs::Inode));
    auto rename_req =
        CreateMDRequest(builder, uid, gid, dfs::mdrequest::MDOpType_Put,
                        inode_id, last_seen_hlc, vec);
    builder.Finish(rename_req);
    buf_send = builder.GetBufferPointer();
    request_size = builder.GetSize();
    memcpy(buf_recv, buf_send, request_size);
    spdlog::info("put request size = {}", request_size);
  }
  { // Phase 2: Unpack request and check
    auto req_recv = dfs::mdrequest::GetMDRequest(buf_recv);
    EXPECT_EQ(req_recv->uid(), 1);
    EXPECT_EQ(req_recv->gid(), 2);
    EXPECT_EQ(req_recv->op(), dfs::mdrequest::MDOpType_Put);
    EXPECT_EQ(req_recv->inode_id(), inode_id);
    EXPECT_EQ(req_recv->last_seen_hlc(), last_seen_hlc);
    EXPECT_TRUE(memcmp(&inode, req_recv->inode()->data(), sizeof(dfs::Inode)) ==
                0);
    spdlog::info("put inode #{:#x} ok", req_recv->inode_id());
  }
}
