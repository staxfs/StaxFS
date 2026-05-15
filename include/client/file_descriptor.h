#pragma once

#include "common/metadata_types.h"
#include <cstdint>
#include <fcntl.h>
#include <queue>
#include <unordered_map>

namespace dfs {

struct FileDescriptor {
  int fd_;                   // file descriptor
  int flags_ = 0;            // file descriptor flags
  bool modify_ = false;      // whether modified
  uint64_t seek_offset_ = 0; // seek offset
  Inode inode_;              // inode

  FileDescriptor() : fd_(-1) {}

  FileDescriptor(int fd, Inode inode) : fd_(fd), inode_(inode) {}
};

class FdManager {
  std::queue<int> fd_pool_;
  int min_free_fd_;

public:
  std::unordered_map<int, FileDescriptor> fd_map_;
  FdManager();
  ~FdManager();
  auto AllocateFd(const Inode &inode) -> int;
  auto ReleaseFd(int fd) -> int;
  auto GetFd(int fd, FileDescriptor &dfs_fd) -> int;
  auto SetFd(int fd, FileDescriptor const &dfs_fd) -> int;
};

auto LocateNodeId(const Inode &inode) -> int;

auto LocateThreadId(const Inode &inode, int node_id) -> int;
} // namespace dfs
