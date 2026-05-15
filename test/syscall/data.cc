#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

auto main() -> int {
  // dfs is mount on /dfs
  int ret = 0;
  int fd = 0;
  ret = mkdir("/dfs/mydir", 0666);
  printf("ret : %d\n", ret);
  ret = creat("/dfs/mydir/test.txt", 0666);
  printf("ret : %d\n", ret);
  ret = creat("/dfs/nodir/test.txt", 0666);
  printf("ret : %d\n", ret);

  fd = open("/dfs/test.txt", O_CREAT | O_RDWR, 0666);
  if (fd < 0) {
    printf("open file error\n");
    return 0;
  }
  std::cout << "fd : " << fd << std::endl;
  ret = 0;
  const char *write_buf = "hello!";

  // test pread,pwrite
  printf("write buf : %s\n", write_buf);
  ret = pwrite(fd, write_buf, 6, 0);
  if (ret < 0) {
    printf("write file error\n");
    return 0;
  }
  char *mybuf = new char[1024];
  memset(mybuf, 0, 1024);
  ret = 0;
  ret = pread(fd, mybuf, 6, 0);
  if (ret < 0) {
    printf("read file error\n");
    return 0;
  }
  printf("read buf : %s\n", mybuf);

  // test lseek,read,write
  for (int i = 0; i < 20; i++) {
    ret = write(fd, write_buf, 6);
  }
  ret = lseek(fd, 1, SEEK_SET);
  for (int i = 0; i < 19; i++) {
    ret = read(fd, mybuf, 6);
    printf("read buf : %s\n", mybuf);
    if (strncmp(mybuf, "ello!h", 6) != 0) {
      printf("read file error\n");
      return -1;
    }
    memset(mybuf, 0, 1024);
  }
  ret = close(fd);
  if (ret < 0) {
    printf("close file error\n");
    return 0;
  }
  fd = open("/dfs/test.txt", O_RDWR);
  if (fd < 0) {
    printf("open file error\n");
    return 0;
  }
  ret = close(fd);
  if (ret < 0) {
    printf("close file error\n");
    return 0;
  }
}