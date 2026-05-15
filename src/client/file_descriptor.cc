#include "spdlog/common.h"
#include <client/file_descriptor.h>
#include <client/g_constants.h>
#include <client/g_preload.h>
#include <common/metadata_types.h>
#include <cstdint>
#include <spdlog/spdlog.h>

namespace dfs {

FdManager::FdManager() { min_free_fd_ = kDfsMagicFdPrefix; }

FdManager::~FdManager() = default;

auto FdManager::AllocateFd(const Inode &inode) -> int {
  int fd;
  if (fd_pool_.empty()) {
    fd = min_free_fd_++;
  } else {
    fd = fd_pool_.front();
    fd_pool_.pop();
  }
  SPDLOG_TRACE("allocate fd {:x} for inode {}", fd, inode.id_);
  fd_map_[fd] = {fd, inode};
  return fd;
}

auto FdManager::ReleaseFd(int fd) -> int {
  if (fd_map_.erase(fd) != 1) {
    SPDLOG_ERROR("Trying to release fd = {}, which is not used.", fd);
    return -1;
  }
  if (fd == min_free_fd_ - 1) {
    min_free_fd_--;
  } else {
    fd_pool_.push(fd);
  }
  return 0;
}

auto FdManager::GetFd(int fd, FileDescriptor &dfs_fd) -> int {
  if (fd_map_.find(fd) == fd_map_.end()) {
    SPDLOG_ERROR("fd {} is not found", fd);
    return -1;
  }
  dfs_fd = fd_map_[fd];
  return 0;
}

auto FdManager::SetFd(int fd, FileDescriptor const &dfs_fd) -> int {
  if (fd_map_.find(fd) == fd_map_.end()) {
    SPDLOG_ERROR("fd {} is not found", fd);
    return -1;
  }
  fd_map_[fd] = dfs_fd;
  return 0;
}

auto LocateNodeId(const Inode &inode) -> int {
  // TODO(wyf) : just a simple demo
  int num_of_dataserver = gPreloadCtx->rpc_client_->GetDataserverCount();
  return std::hash<uint64_t>{}(inode.id_) % num_of_dataserver;
}

auto LocateThreadId(const Inode &inode, int node_id) -> int {
  int num_of_thread =
      gPreloadCtx->rpc_client_->GetDataserverThreadCount(node_id);
  return std::hash<uint64_t>{}(inode.id_) % num_of_thread;
}
} // namespace dfs
