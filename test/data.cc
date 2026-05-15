#include "server/data.h"
#include "server/metadata.h"

// Stub for gMetaManager (defined in server_main.cc, pulled in by
// dfs-prototype-server-lib → rpc_server.cc)
std::unique_ptr<dfs::Metadata> gMetaManager = nullptr;

#include <cstring>
#include <gtest/gtest.h>

TEST(InitTest, Smoke) {
  auto data = dfs::ObjectStore::Init("/tmp/dfs_test_data");
  std::string buffer1 = "hello world";
  std::string buffer2 = "j90bvswem054g8jmnszlfdj09";
  char *temp_check_buffer = new char[100];

  data->Write(1, 0, buffer1.size(), buffer1.c_str());
  data->Read(1, 0, buffer1.size(), temp_check_buffer);
  ASSERT_EQ(memcmp(buffer1.c_str(), temp_check_buffer, buffer1.size()), 0);

  data->Write(1, 50, buffer1.size(), buffer1.c_str());
  data->Read(1, 50, buffer1.size(), temp_check_buffer);
  ASSERT_EQ(memcmp(buffer1.c_str(), temp_check_buffer, buffer1.size()), 0);

  data->Write(987123546, 0, buffer1.size(), buffer1.c_str());
  data->Write(987123546, buffer1.size(), buffer1.size() + buffer2.size(),
              buffer2.c_str());
  data->Read(987123546, 0, buffer1.size() + buffer2.size(), temp_check_buffer);
  std::string buffer12 = buffer1 + buffer2;
  ASSERT_EQ(memcmp(buffer12.c_str(), temp_check_buffer, buffer1.size()), 0);

  delete[] temp_check_buffer;
  std::cout << "Dummy Test DATA OK" << std::endl;
}