#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
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

auto main() -> int { // Adapted from ProcessFunction in test/metadata.cc
  int ret;
  // mkdir
  ret = mkdir(DFS_MNT "/tmp", 0755);
  ASSERT_EQ(ret, 0);
  ret = mkdir(DFS_MNT "/tmp/not/exist", 0755);
  ASSERT_EQ(ret, -1);
  // access()
  ret = access(DFS_MNT "/tmp", F_OK);
  EXPECT_EQ(ret, 0);
  ret = access(DFS_MNT "/tmp", W_OK | R_OK | X_OK);
  EXPECT_EQ(ret, 0);
  // stat()
  struct stat tmp;
  ret = stat(DFS_MNT "/tmp", &tmp);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(tmp.st_mode, 0100755);
  ret = stat(DFS_MNT "/test", &tmp);
  EXPECT_EQ(ret, -1);
  // rename()
  ret = rename(DFS_MNT "/tmp", DFS_MNT "/test");
  ASSERT_EQ(ret, 0);
  ret = access(DFS_MNT "/tmp", F_OK);
  EXPECT_EQ(ret, -1);
  ret = access(DFS_MNT "/test", F_OK);
  EXPECT_EQ(ret, 0);
  // create()
  ret = creat(DFS_MNT "/test/file.txt", 0666);
  ASSERT_EQ(ret, 0xfff0000);
  ret = stat(DFS_MNT "/test/file.txt", &tmp);
  printf("created inode_id = %lu\n", tmp.st_ino);
  EXPECT_EQ(tmp.st_mode, 0666);
  ret = creat(DFS_MNT "/tmp/file.txt", 0666);
  EXPECT_EQ(ret, -1);
  ret = access(DFS_MNT "/test/file.txt", F_OK);
  EXPECT_EQ(ret, 0);
  // link()
  ret = link(DFS_MNT "/test/file.txt", DFS_MNT "/file1.txt");
  ASSERT_EQ(ret, 0);

  // opendir()
  DIR *dstream = opendir(DFS_MNT "/");
  if (dstream == nullptr) {
    printf("dstream is null!\n");
    return -1;
  }
  EXPECT_EQ((uint64_t)dstream & 1, 1);
  // readdir()
  dirent *dptr;
  ret = 0;
  while ((dptr = readdir(dstream)) != nullptr && ret < 5) {
    ret++;
    printf("Entry #%d: id = %lu, name = %s, reclen = %u\n", ret, dptr->d_ino,
           dptr->d_name, dptr->d_reclen);
  }
  EXPECT_EQ(ret, 2);

  closedir(dstream);

  // unlink()
  ret = unlink(DFS_MNT "/test/file.txt");
  ASSERT_EQ(ret, 0);
  ret = access(DFS_MNT "/test/file.txt", F_OK);
  EXPECT_EQ(ret, -1);
  ret = access(DFS_MNT "/file1.txt", F_OK);
  EXPECT_EQ(ret, 0);
  ret = unlink(DFS_MNT "/test");
  ASSERT_EQ(ret, -1);
  ret = unlink(DFS_MNT "/file1.txt");
  ASSERT_EQ(ret, 0);
  // rmdir()
  ret = rmdir(DFS_MNT "/test");
  EXPECT_EQ(ret, 0);
  ret = access(DFS_MNT "/test", F_OK);
  EXPECT_EQ(ret, -1);

  printf("ALL TESTS FINISHED!\n");
}