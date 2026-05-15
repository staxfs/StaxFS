#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define ASSERT_EQ(v1, v2)                                                      \
  if ((v1) != (v2)) {                                                          \
    std::cout << "ASSERT FAILED: " #v1 " is " << (v1) << " != " #v2 "\n";      \
    return -1;                                                                 \
  }

#define EXPECT_EQ(v1, v2)                                                      \
  if ((v1) != (v2)) {                                                          \
    std::cout << "EXPECT FAILED: " #v1 " is " << (v1) << " != " #v2 "\n";      \
  }

#define DFS_MNT "/dfs"

int main() {
  const char *node_rank = getenv("OMPI_COMM_WORLD_NODE_RANK");
  std::string dirname(DFS_MNT "/");
  dirname += node_rank;
  int ret = mkdir(dirname.c_str(), 0755);
  ASSERT_EQ(ret, 0);
  auto filename = dirname + "/file";
  ret = creat(filename.c_str(), 0666);
  ASSERT_EQ(ret, 0xfff0000);
  ret = unlink(filename.c_str());
  EXPECT_EQ(ret, 0);
  ret = rmdir(dirname.c_str());
  EXPECT_EQ(ret, 0);
  return 0;
}