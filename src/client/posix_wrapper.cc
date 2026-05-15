#include "client/posix_wrapper.h"

#include "client/g_preload.h"
#include "client/preload.h"
#include "client/rpc_client.h"
#include "common/listdir_profile.h"
#include "common/metadata_types.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <asm-generic/errno-base.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <memory>
#include <sys/mman.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

using ssize_t = int64_t;
using size_t = uint64_t;

std::unordered_map<uint64_t, std::list<LockEntry>> gLockTable;
std::mutex gLockTableMutex;

static auto GetStreamFdMap() -> std::unordered_map<FILE *, int> & {
  static std::unordered_map<FILE *, int> map;
  return map;
}

static auto GetStreamFdMapMutex() -> std::mutex & {
  static std::mutex mtx;
  return mtx;
}

static auto GetMmapMap() -> std::unordered_map<void *, int> & {
  static std::unordered_map<void *, int> map;
  return map;
}

static auto GetMmapMapMutex() -> std::mutex & {
  static std::mutex mtx;
  return mtx;
}

extern "C" {

auto LocksConflict(const struct flock *a, const struct flock *b) -> bool {
  return a->l_type == F_WRLCK || b->l_type == F_WRLCK;
}

auto dfs_open(const char *pathname, int flags, mode_t mode) -> int {
  if ((flags & O_CREAT) != 0) {
    SPDLOG_TRACE("hook : dfs_open(pathname={},flags={:b},mode={:o}) -> creat ",
                 pathname, flags, mode);
    return dfs_creat(pathname, mode);
  }
  SPDLOG_TRACE("hook : dfs_open(pathname={},flags={:b},mode={:o})", pathname,
               flags, mode);
  dfs::Inode node;
  int ret = dfs::gPreloadCtx->rpc_client_->Stat(pathname, &node);
  if (ret != 0) { // file doesnt exist
    errno = ENOENT;
    return ret;
  }
  int fd = dfs::gPreloadCtx->fd_manager_->AllocateFd(node);
  return fd;
}

auto dfs_lseek(int fd, int64_t offset, int whence) -> int64_t {
  SPDLOG_TRACE("hook : dfs_lseek(fd={:x},offset={},whence={})", fd, offset,
               whence);
  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    return ret;
  }
  // https://linux.die.net/man/2/lseek
  if (whence == SEEK_SET) {
    SPDLOG_TRACE("SEEK_SET");
    dfs_fd.seek_offset_ = offset;
    dfs::gPreloadCtx->fd_manager_->SetFd(fd, dfs_fd);
    return dfs_fd.seek_offset_;
  }
  if (whence == SEEK_CUR) {
    dfs_fd.seek_offset_ += offset;
    dfs::gPreloadCtx->fd_manager_->SetFd(fd, dfs_fd);
    return dfs_fd.seek_offset_;
  }
  if (whence == SEEK_END) {
    dfs_fd.seek_offset_ = dfs_fd.inode_.size_ + offset;
    dfs::gPreloadCtx->fd_manager_->SetFd(fd, dfs_fd);
    return dfs_fd.seek_offset_;
  }
  if (whence == SEEK_DATA || whence == SEEK_HOLE) {
    errno = EINVAL;
    return -1;
  }
  return -1;
}

auto dfs_read(int fd, void *buf, size_t count) -> ssize_t {
  DTRACE_PROBE1(DFS_CLIENT, dfs_read_enter, count);
  SPDLOG_TRACE("hook : dfs_read(fd={:x},count={})", fd, count);

  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    return ret;
  }
  int node_id = dfs::LocateNodeId(dfs_fd.inode_);
  uint64_t offset = dfs_fd.seek_offset_;
  uint64_t objuuid = dfs_fd.inode_.id_;
  if (dfs_fd.inode_.size_ == offset) {
    ret = 0;
  } else {
    ret = dfs::gPreloadCtx->rpc_client_->RpcPread(
        node_id, objuuid, offset, std::min(count, dfs_fd.inode_.size_ - offset),
        buf);
  }
  SPDLOG_TRACE(
      "Read: fd: {}, node_id_ : {}, objuuid : {}, offset : {}, count : "
      "{}, read_size = {}",
      fd, node_id, objuuid, offset, count, ret);
  if (ret < 0) {
    return ret;
  }

  // update global fd map
  if (ret != 0) {
    dfs_fd.seek_offset_ += ret;
    dfs::gPreloadCtx->fd_manager_->SetFd(fd, dfs_fd);
  }

  DTRACE_PROBE(DFS_CLIENT, dfs_read_ret);
  return ret;
}

auto dfs_write(int fd, const void *buf, size_t count) -> ssize_t {
  DTRACE_PROBE1(DFS_CLIENT, dfs_write_enter, count);
  SPDLOG_TRACE("hook : dfs_write(fd={:x},count={})", fd, count);

  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    return ret;
  }

  int node_id = dfs::LocateNodeId(dfs_fd.inode_);
  uint64_t offset = dfs_fd.seek_offset_;
  uint64_t objuuid = dfs_fd.inode_.id_;
  SPDLOG_TRACE("Write: fd: {}, node_id_ : {}, objuuid : {}, offset : {}, count"
               ": {}, inode.size = {}",
               fd, node_id, objuuid, offset, count, dfs_fd.inode_.size_);
  ret = dfs::gPreloadCtx->rpc_client_->RpcPwrite(node_id, objuuid, offset,
                                                 count, buf);
  if (ret < 0) {
    return ret;
  }

  // update global fd map
  if (ret != 0) {
    dfs_fd.seek_offset_ += ret;
    dfs_fd.inode_.size_ += ret;
    dfs_fd.modify_ = true;
    dfs::gPreloadCtx->fd_manager_->SetFd(fd, dfs_fd);
  }
  SPDLOG_TRACE(
      "Write: offset = {}, inode.id = {}, inode.size_ = {}, modify = {}",
      dfs_fd.seek_offset_, dfs_fd.inode_.id_, dfs_fd.inode_.size_,
      dfs_fd.modify_);

  DTRACE_PROBE(DFS_CLIENT, dfs_write_ret);
  return ret;
}

auto dfs_pread(int fd, void *buf, uint64_t count, int64_t offset) -> int64_t {
  DTRACE_PROBE2(DFS_CLIENT, dfs_pread_enter, count, offset);
  SPDLOG_TRACE("hook : dfs_pread(fd={:x},count={},offset={})", fd, count,
               offset);

  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    return ret;
  }

  int node_id = dfs::LocateNodeId(dfs_fd.inode_);
  uint64_t objuuid = dfs_fd.inode_.id_;
  SPDLOG_TRACE("node_id_ : {}, objuuid : {}, offset : {}, count : {}", node_id,
               objuuid, offset, count);
  if (dfs_fd.inode_.size_ == offset) {
    ret = 0;
  } else {
    ret = dfs::gPreloadCtx->rpc_client_->RpcPread(node_id, objuuid, offset,
                                                  count, buf);
  }
  if (ret < 0) {
    return ret;
  }
  DTRACE_PROBE(DFS_CLIENT, dfs_pread_ret);
  return ret;
}

auto dfs_pwrite(int fd, const void *buf, uint64_t count, int64_t offset)
    -> int64_t {
  DTRACE_PROBE2(DFS_CLIENT, dfs_pwrite_enter, count, offset);
  SPDLOG_TRACE("hook : dfs_pwrite(fd={:x},count={},offset={})", fd, count,
               offset);

  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    return ret;
  }
  int node_id = dfs::LocateNodeId(dfs_fd.inode_);
  uint64_t objuuid = dfs_fd.inode_.id_;
  SPDLOG_TRACE("node_id_ : {}, objuuid : {}, offset : {}, count : {}", node_id,
               objuuid, offset, count);
  ret = dfs::gPreloadCtx->rpc_client_->RpcPwrite(node_id, objuuid, offset,
                                                 count, buf);
  if (ret < 0) {
    return ret;
  }

  if (ret != 0) {
    dfs_fd.inode_.size_ += ret;
    dfs_fd.modify_ = true;
    dfs::gPreloadCtx->fd_manager_->SetFd(fd, dfs_fd);
  }
  DTRACE_PROBE(DFS_CLIENT, dfs_pwrite_ret);
  return ret;
}

auto dfs_fsync(int fd) -> int {
  SPDLOG_TRACE("hook : dfs_fsync(fd={:x})", fd);
  // TODO(wyf)
  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    return ret;
  }
  SPDLOG_TRACE("Sync: fd = {}, inode.id = {}, inode.size_ = {}, modify = {}",
               fd, dfs_fd.inode_.id_, dfs_fd.inode_.size_, dfs_fd.modify_);
  if (dfs_fd.modify_) {
    dfs::gPreloadCtx->rpc_client_->PutInodeRequest(dfs_fd.inode_.id_,
                                                   &dfs_fd.inode_);
    dfs_fd.modify_ = false;
    dfs::gPreloadCtx->fd_manager_->SetFd(fd, dfs_fd);
  }
  return 0;
}

auto dfs_fdatasync(int fd) -> int {
  SPDLOG_TRACE("hook : dfs_fdatasync(fd={:x})", fd);
  return dfs_fsync(fd);
}

auto dfs_close(int fd) -> int {
  SPDLOG_TRACE("hook : dfs_close(fd={:x})", fd);
  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    return ret;
  }
  SPDLOG_TRACE("Close: fd = {}, inode.id = {}, inode.size_ = {}, modify = {}",
               fd, dfs_fd.inode_.id_, dfs_fd.inode_.size_, dfs_fd.modify_);
  if (dfs_fd.modify_) {
    dfs::gPreloadCtx->rpc_client_->PutInodeRequest(dfs_fd.inode_.id_,
                                                   &dfs_fd.inode_);
  }
  dfs::gPreloadCtx->fd_manager_->ReleaseFd(fd);
  return 0;
}

auto dfs_dup(int oldfd) -> int {
  SPDLOG_WARN("hook : dfs_dup(oldfd={:x}) unimplemented", oldfd);
  // TODO(wyf)
  return 0;
}

auto dfs_dup3(int oldfd, int newfd, int flags) -> int {
  SPDLOG_WARN("hook : dfs_dup3(oldfd={:x},newfd={:x},flags={:b}) unimplemented",
              oldfd, newfd, flags);
  // TODO(wyf)
  return 0;
}

auto dfs_fcntl(int fd, int cmd, va_list args) -> int {
  SPDLOG_TRACE("hook : dfs_fcntl(fd={:x},cmd={})", fd, cmd);

  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    SPDLOG_ERROR("dfs_fcntl: invalid file descriptor {}", fd);
    return ret;
  }

  switch (cmd) {
  case F_GETFD: {
    // return file descriptor flags
    SPDLOG_TRACE("F_GETFD: returning flags {}", dfs_fd.flags_);
    return dfs_fd.flags_;
  }

  case F_SETFD: {
    // set file descriptor flags
    int flags = va_arg(args, int);
    SPDLOG_TRACE("F_SETFD: setting flags to {}", flags);
    dfs_fd.flags_ = flags;
    dfs::gPreloadCtx->fd_manager_->SetFd(fd, dfs_fd);
    return 0;
  }

  case F_GETLK: {
    // get file lock information
    struct flock *lock = va_arg(args, struct flock *);
    SPDLOG_TRACE("F_GETLK: checking lock status");

    std::lock_guard<std::mutex> lock_guard(gLockTableMutex);
    auto &locks = gLockTable[dfs_fd.inode_.id_];
    for (const auto &entry : locks) {
      if (LocksConflict(lock, &entry.lock_)) {
        *lock = entry.lock_;
        return 0;
      }
    }
    lock->l_type = F_UNLCK;
    return 0;
  }

  case F_SETLK:
  case F_SETLKW: {
    struct flock *lock = va_arg(args, struct flock *);

    bool blocking = (cmd == F_SETLKW);
    std::unique_lock<std::mutex> lk(gLockTableMutex);

    // check lock type
    if (lock->l_type != F_UNLCK && lock->l_type != F_RDLCK &&
        lock->l_type != F_WRLCK) {
      SPDLOG_ERROR("F_SETLK: invalid lock type {}", lock->l_type);
      errno = EINVAL;
      return -1;
    }

    // unlock
    if (lock->l_type == F_UNLCK) {
      auto &locks = gLockTable[dfs_fd.inode_.id_];
      for (auto it = locks.begin(); it != locks.end(); ++it) {
        if (it->pid_ == getpid() && LocksConflict(lock, &it->lock_)) {
          locks.erase(it);
          return 0;
        }
      }
      errno = EACCES;
      return -1;
    }

    // lock
    while (true) {
      bool conflict = false;
      auto &locks = gLockTable[dfs_fd.inode_.id_];
      for (const auto &entry : locks) {
        if (LocksConflict(lock, &entry.lock_)) {
          conflict = true;
          break;
        }
      }

      if (!conflict) {
        // lock success
        locks.emplace_back(getpid(), *lock);
        return 0;
      }

      if (!blocking) {
        // non-blocking mode failed
        errno = EACCES;
        return -1;
      }

      // blocking mode: wait for lock release
      lk.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      lk.lock();
    }
  }

  default: {
    SPDLOG_WARN("Unsupported fcntl command: {}", cmd);
    errno = EINVAL;
    return -1;
  }
  }
}

auto dfs_stat(const char *pathname, struct stat *statbuf) -> int {
  SPDLOG_TRACE("hook : dfs_stat(pathname={})", pathname);
  DTRACE_PROBE1(DFS_CLIENT, dfs_stat_enter, pathname);
  dfs::Inode node;
  int ret = dfs::gPreloadCtx->rpc_client_->Stat(pathname, &node);
  if (ret != 0) {
    errno = ENOENT;
    return ret;
  }
  statbuf->st_dev = 10000;
  statbuf->st_ino = node.id_;
  statbuf->st_nlink = node.nlink_;
  statbuf->st_mode = node.mode_;
  statbuf->st_gid = node.gid_;
  statbuf->st_uid = node.uid_;
  statbuf->st_size = node.size_;
  statbuf->st_blocks = node.blocks_;
  statbuf->st_blksize = node.blksize_;
  statbuf->st_atime = node.atime_;
  statbuf->st_ctime = node.ctime_;
  statbuf->st_mtime = node.mtime_;
  // rdev are not used, so I omitted it.

  DTRACE_PROBE1(DFS_CLIENT, dfs_stat_ret, node.id_);
  return 0;
}

auto dfs_fstat(int fd, struct stat *statbuf) -> int {
  SPDLOG_TRACE("hook : dfs_fstat(fd={:x})", fd);
  DTRACE_PROBE1(DFS_CLIENT, dfs_fstat_enter, fd);
  int ret = 0;
  dfs::FileDescriptor dfs_fd;
  ret = dfs::gPreloadCtx->fd_manager_->GetFd(fd, dfs_fd);
  if (ret < 0) {
    errno = ENOENT;
    return ret;
  }
  statbuf->st_dev = 10000;
  statbuf->st_ino = dfs_fd.inode_.id_;
  statbuf->st_nlink = dfs_fd.inode_.nlink_;
  statbuf->st_mode = dfs_fd.inode_.mode_;
  statbuf->st_gid = dfs_fd.inode_.gid_;
  statbuf->st_uid = dfs_fd.inode_.uid_;
  statbuf->st_size = dfs_fd.inode_.size_;
  statbuf->st_blocks = dfs_fd.inode_.blocks_;
  statbuf->st_blksize = dfs_fd.inode_.blksize_;
  statbuf->st_atime = dfs_fd.inode_.atime_;
  statbuf->st_ctime = dfs_fd.inode_.ctime_;
  statbuf->st_mtime = dfs_fd.inode_.mtime_;
  // rdev are not used, so I omitted it.

  DTRACE_PROBE1(DFS_CLIENT, dfs_fstat_ret, dfs_fd.inode_.id_);
  return ret;
}

auto dfs_access(const char *pathname, int mode) -> int {
  SPDLOG_TRACE("hook : dfs_access(pathname={},mode={:b})", pathname, mode);
  int ret = dfs::gPreloadCtx->rpc_client_->Access(pathname, mode);
  if (ret != 0) {
    errno = ENOENT;
  }
  return ret;
}

auto dfs_rename(const char *oldpath, const char *newpath) -> int {
  SPDLOG_TRACE("hook : dfs_rename(oldpath={},newpath={})", oldpath, newpath);
  return dfs::gPreloadCtx->rpc_client_->Rename(oldpath, newpath);
}

auto dfs_unlink(const char *pathname) -> int {
  SPDLOG_TRACE("hook : dfs_unlink(pathname={})", pathname);
  return dfs::gPreloadCtx->rpc_client_->Unlink(pathname);
}

auto dfs_mkdir(const char *pathname, uint32_t mode) -> int {
  SPDLOG_TRACE("hook : dfs_mkdir(pathname={},mode={:o})", pathname, mode);
  int ret = dfs::gPreloadCtx->rpc_client_->Mkdir(pathname, mode);
  return ret;
}

auto dfs_rmdir(const char *pathname) -> int {
  SPDLOG_TRACE("hook : dfs_rmdir(pathname={})", pathname);
  return dfs::gPreloadCtx->rpc_client_->Rmdir(pathname);
}

auto dfs_link(const char *oldpath, const char *newpath) -> int {
  SPDLOG_TRACE("hook : dfs_link(oldpath={}, newpath={})", oldpath, newpath);
  return dfs::gPreloadCtx->rpc_client_->Link(oldpath, newpath);
}

struct ClientDirstream {
  DirHandle handle_{};
  size_t read_off_ = 0;
  size_t merged_size_ = 0;
  char *merged_buf_ = nullptr;
};

static inline auto EncodeDfsDirstream(ClientDirstream *sptr) -> DIR * {
  return reinterpret_cast<DIR *>(reinterpret_cast<std::uintptr_t>(sptr) | 1);
}

static inline auto DecodeDfsDirstream(DIR *sptr) -> ClientDirstream * {
  return reinterpret_cast<ClientDirstream *>(
      reinterpret_cast<std::uintptr_t>(sptr) & ~1ULL);
}

// Walk a reclen-delimited DentView stream once to check reclen/offset
// consistency. No records are copied — the k-way merge consumes the stream
// in place from its per-meta accumulation buffer.
static auto ValidateDentViewStream(const void *buf, uint32_t size) -> bool {
  size_t pos = 0;
  while (pos + offsetof(DentView, name_) <= size) {
    auto *view =
        dfs::OffsetDirent(reinterpret_cast<DentView *>(const_cast<void *>(buf)),
                          pos);
    if (view->reclen_ < offsetof(DentView, name_) + 1 ||
        pos + view->reclen_ > size) {
      return false;
    }
    pos += view->reclen_;
  }
  return pos == size;
}

static auto LoadMergedDirents(ClientDirstream *sptr, uint32_t chunk_size)
    -> bool {
  LISTDIR_PROFILE_SCOPE(dfs::kCliLoadMergedTotal);
  const int meta_count =
      dfs::gPreloadCtx->rpc_client_->GetMetaserverCount();

  // Per-meta accumulated byte streams. Each meta's GetDentViews response is
  // already sorted by name asc (server-side CollectPersistentDentViews sorts
  // before SerializeDentViews), so we preserve that shape and do a k-way
  // merge at the end instead of re-sorting a flat concatenation.
  //
  // For the common single-page case streams[m] is filled by exactly one
  // insert; pagination (a directory larger than chunk_size on one meta)
  // triggers additional appends that keep the stream globally sorted because
  // the server paginates within its own sorted sequence.
  //
  // Thread-local reuse: the outer vector and each inner byte buffer grow to
  // the high-water mark on first use and are then reused across every
  // listdir on thisrves  client thread. `clear()` preseinner capacity so
  // subsequent `insert`s are amortized memcpy with zero heap traffic.
  thread_local std::vector<std::vector<char>> tl_streams;
  if (static_cast<int>(tl_streams.size()) < meta_count) {
    tl_streams.resize(meta_count);
  }
  for (int m = 0; m < meta_count; ++m) {
    tl_streams[m].clear();
  }
  auto &streams = tl_streams;

  // Transient scratch for each RPC round — one chunk_size slot per meta.
  // Contents are consumed and copied into streams[m] before the next round.
  //
  // Thread-local reuse avoids a multi-MB zero-init on every listdir: for
  // chunk_size=DIR_MAX_ALLOC (1 MB) × 3 metas the previous per-call
  // std::vector<char>(N) constructor paid ~250 μs in memset + page faults.
  // Growth-only resize means the first call touches 3 MB of pages; every
  // subsequent call on this thread is a no-op.
  thread_local std::vector<char> tl_scratch;
  const size_t scratch_need =
      static_cast<size_t>(meta_count) * chunk_size;
  if (tl_scratch.size() < scratch_need) {
    tl_scratch.resize(scratch_need);
  }
  auto &scratch = tl_scratch;
  std::vector<off_t> offsets(meta_count, 0);
  std::vector<char> done(meta_count, 0);
  std::vector<int> active_metas;
  std::vector<off_t> active_offsets;
  std::vector<char *> active_bufs;
  std::vector<int> read_sizes;
  active_metas.reserve(meta_count);
  active_offsets.reserve(meta_count);
  active_bufs.reserve(meta_count);
  read_sizes.reserve(meta_count);

  while (true) {
    active_metas.clear();
    active_offsets.clear();
    active_bufs.clear();
    for (int m = 0; m < meta_count; ++m) {
      if (done[m] == 0) {
        active_metas.push_back(m);
        active_offsets.push_back(offsets[m]);
        active_bufs.push_back(scratch.data() +
                              static_cast<size_t>(m) * chunk_size);
      }
    }
    if (active_metas.empty()) {
      break;
    }
    read_sizes.assign(active_metas.size(), 0);

    int batch_ret;
    {
      LISTDIR_PROFILE_SCOPE(dfs::kCliGetDentViewsRpc);
      batch_ret = dfs::gPreloadCtx->rpc_client_->BatchGetDentViews(
          sptr->handle_.id_, sptr->handle_.read_cutoff_version_,
          static_cast<int>(active_metas.size()), active_metas.data(),
          active_offsets.data(), active_bufs.data(), chunk_size,
          read_sizes.data());
    }
    if (batch_ret < 0) {
      for (size_t i = 0; i < active_metas.size(); ++i) {
        if (read_sizes[i] < 0) {
          SPDLOG_ERROR("LoadMergedDirents: GetDentViews RPC failed for dir={} "
                       "meta={} local_offset={}",
                       sptr->handle_.id_, active_metas[i],
                       static_cast<long>(active_offsets[i]));
        }
      }
      return false;
    }

    for (size_t i = 0; i < active_metas.size(); ++i) {
      const int m = active_metas[i];
      const int rs = read_sizes[i];
      if (rs > 0) {
        bool stream_ok;
        {
          // Validate + append this round's payload into the per-meta stream.
          // Common single-page case: streams[m] was empty so the insert is a
          // single memcpy of `rs` bytes.
          LISTDIR_PROFILE_SCOPE(dfs::kCliMergeBuffer);
          stream_ok = ValidateDentViewStream(active_bufs[i],
                                             static_cast<uint32_t>(rs));
          if (stream_ok) {
            streams[m].insert(streams[m].end(), active_bufs[i],
                              active_bufs[i] + rs);
          }
        }
        if (!stream_ok) {
          SPDLOG_ERROR("LoadMergedDirents: malformed DentView stream for "
                       "dir={} meta={} local_offset={} read_size={}",
                       sptr->handle_.id_, m,
                       static_cast<long>(active_offsets[i]), rs);
          return false;
        }
      }
      offsets[m] += rs;
      if (rs == 0) {
        done[m] = 1;
      }
    }
  }

  // K-way merge over the per-meta sorted streams. With the shard-by-create-
  // MDS layout a single name usually appears on exactly one meta, so the
  // inner "collision resolution" loop almost always sees zero matches. When
  // a name does appear on multiple metas (e.g. across rename history) the
  // winning op is the highest version; tombstones suppress any matching
  // live copies because after advance the losing streams are past that
  // name. Complexity: O(total_entries × meta_count), which for k=3 metas is
  // comfortably cheaper than the previous O(n log n) sort.
  std::vector<char> packed;
  {
    LISTDIR_PROFILE_SCOPE(dfs::kCliSortAndPack);

    struct Cursor {
      const char *cur;
      const char *end;
    };
    std::vector<Cursor> cursors;
    cursors.reserve(meta_count);
    size_t total_bytes = 0;
    for (int m = 0; m < meta_count; ++m) {
      if (!streams[m].empty()) {
        cursors.push_back({streams[m].data(),
                           streams[m].data() + streams[m].size()});
        total_bytes += streams[m].size();
      }
    }

    auto view_at = [](const Cursor &c) {
      return reinterpret_cast<const DentView *>(c.cur);
    };
    auto valid = [](const Cursor &c) { return c.cur < c.end; };

    // Upper bound on output: each output Dirent's reclen is smaller than its
    // source DentView's reclen, so total_bytes is a generous reservation.
    packed.reserve(total_bytes);

    const size_t k = cursors.size();
    while (true) {
      // Find the cursor whose current record has the smallest name.
      int min_idx = -1;
      for (size_t i = 0; i < k; ++i) {
        if (!valid(cursors[i])) {
          continue;
        }
        if (min_idx < 0 ||
            std::strcmp(view_at(cursors[i])->name_,
                        view_at(cursors[min_idx])->name_) < 0) {
          min_idx = static_cast<int>(i);
        }
      }
      if (min_idx < 0) {
        break;
      }

      // Resolve cross-meta name collisions by keeping the highest-version op.
      // `winner` stays valid throughout the loop because stream memory is
      // owned by `streams` and never resized after cursor setup.
      const DentView *winner = view_at(cursors[min_idx]);
      for (size_t i = 0; i < k; ++i) {
        if (static_cast<int>(i) == min_idx || !valid(cursors[i])) {
          continue;
        }
        const DentView *v = view_at(cursors[i]);
        if (std::strcmp(v->name_, winner->name_) != 0) {
          continue;
        }
        if (v->version_ > winner->version_) {
          winner = v;
        }
      }

      if (!winner->IsDeleted()) {
        Dirent dent(winner->id_, winner->pid_, winner->type_, winner->name_);
        dent.SetRecLen();
        size_t old_size = packed.size();
        packed.resize(old_size + dent.reclen_);
        std::memcpy(packed.data() + old_size, &dent, dent.reclen_);
      }

      // Advance every cursor currently sitting on the winning name. Safe to
      // read winner->name_ here because none of the advances we issue moves
      // past memory we still need to compare against.
      for (size_t i = 0; i < k; ++i) {
        if (!valid(cursors[i])) {
          continue;
        }
        const DentView *v = view_at(cursors[i]);
        if (std::strcmp(v->name_, winner->name_) == 0) {
          cursors[i].cur += v->reclen_;
        }
      }
    }
  }

  {
    LISTDIR_PROFILE_SCOPE(dfs::kCliAllocMergedBuf);
    free(sptr->merged_buf_);
    sptr->merged_buf_ = nullptr;
    sptr->merged_size_ = packed.size();
    sptr->read_off_ = 0;
    if (packed.empty()) {
      return true;
    }
    sptr->merged_buf_ = static_cast<char *>(malloc(packed.size()));
    if (sptr->merged_buf_ == nullptr) {
      SPDLOG_ERROR("LoadMergedDirents: failed to allocate merged snapshot for "
                   "dir={}, size={}",
                   sptr->handle_.id_, packed.size());
      sptr->merged_size_ = 0;
      return false;
    }
    memcpy(sptr->merged_buf_, packed.data(), packed.size());
  }
  return true;
}

auto dfs_opendir(const char *path, int alloc) -> DIR * {
  LISTDIR_PROFILE_SCOPE(dfs::kCliOpendirTotal);
  auto *sptr = static_cast<ClientDirstream *>(calloc(1, sizeof(ClientDirstream)));
  if (sptr == nullptr) {
    return nullptr;
  }
  uint32_t chunk_size =
      std::max<uint32_t>(alloc > 0 ? static_cast<uint32_t>(alloc)
                                   : static_cast<uint32_t>(DIR_MAX_ALLOC),
                         sizeof(DentView));
  int ret;
  {
    LISTDIR_PROFILE_SCOPE(dfs::kCliOpendirRpc);
    ret = dfs::gPreloadCtx->rpc_client_->OpenDir(path, &sptr->handle_);
  }
  if (ret != 0) {
    free(sptr);
    return nullptr;
  }
  if (!LoadMergedDirents(sptr, chunk_size)) {
    free(sptr);
    return nullptr;
  }
  SPDLOG_TRACE("stream: [id={}, size={}]", sptr->handle_.id_,
               sptr->merged_size_);
  // TODO(): Initialize sptr->fd here if needed.
  return EncodeDfsDirstream(sptr);
}

auto dfs_readdir(DIR *_sptr) -> dirent * {
  static dirent conversion;
  auto *sptr = DecodeDfsDirstream(_sptr);
  if (sptr == nullptr || sptr->read_off_ >= sptr->merged_size_) {
    return nullptr;
  }
  if (sptr->read_off_ + offsetof(Dirent, name_) > sptr->merged_size_) {
    SPDLOG_ERROR("dfs_readdir: malformed merged snapshot for dir={}, "
                 "read_off={} merged_size={}",
                 sptr->handle_.id_, sptr->read_off_, sptr->merged_size_);
    return nullptr;
  }
  auto *eptr = dfs::OffsetDirent(reinterpret_cast<Dirent *>(sptr->merged_buf_),
                                 sptr->read_off_);
  if (eptr->reclen_ < offsetof(Dirent, name_) + 1 ||
      sptr->read_off_ + eptr->reclen_ > sptr->merged_size_) {
    SPDLOG_ERROR("dfs_readdir: malformed merged dirent for dir={}, "
                 "read_off={} reclen={} merged_size={}",
                 sptr->handle_.id_, sptr->read_off_, eptr->reclen_,
                 sptr->merged_size_);
    return nullptr;
  }
  sptr->read_off_ += eptr->reclen_;
  eptr->ToLinuxDirent(conversion);
  return &conversion;
}

auto dfs_closedir(DIR *_sptr) -> int {
  auto *sptr = DecodeDfsDirstream(_sptr);
  if (sptr == nullptr) {
    return -1;
  }
  free(sptr->merged_buf_);
  free(sptr);
  // Periodic profile dump on the client side. Interval chosen so filebench
  // workloads (tens of thousands of listdirs) emit a handful of reports.
  LISTDIR_PROFILE_MAYBE_DUMP(5000, "client-periodic");
  return 0;
}

auto dfs_creat(const char *pathname, uint32_t mode) -> int {
  SPDLOG_TRACE("hook : dfs_creat(pathname={},mode={:o})", pathname, mode);
  DTRACE_PROBE2(DFS_CLIENT, dfs_creat_enter, pathname, mode);
  dfs::Inode node;
  int ret = dfs::gPreloadCtx->rpc_client_->Create(pathname, mode, &node);
  SPDLOG_TRACE("creat ret : {}", ret);
  if (ret != 0) {
    errno = ENOENT;
    return -1;
  }
  SPDLOG_TRACE("create: inode id = {}, file size : {}", node.id_, node.size_);
  // allocate fd and return;
  int fd = dfs::gPreloadCtx->fd_manager_->AllocateFd(node);

  DTRACE_PROBE1(DFS_CLIENT, dfs_creat_ret, fd);
  return fd;
}

auto dfs_statx(int dirfd, const char *pathname, int flags, unsigned int mask,
               struct statx *statxbuf) -> int {
  if (pathname[0] != '/') {
    SPDLOG_WARN("dfs_statx for relative path unimplemented !");
  }
  dfs::Inode node;
  int ret = dfs::gPreloadCtx->rpc_client_->Stat(pathname, &node);
  if (ret != 0) {
    return ret;
  }
  statxbuf->stx_ino = node.id_;
  statxbuf->stx_nlink = node.nlink_;
  statxbuf->stx_blksize = node.blksize_;
  statxbuf->stx_blocks = node.blocks_;
  statxbuf->stx_gid = node.gid_;
  statxbuf->stx_uid = node.uid_;
  statxbuf->stx_mode = node.mode_;
  statxbuf->stx_size = node.size_;
  statxbuf->stx_rdev_major = node.rdev_;
  statxbuf->stx_atime.tv_sec = node.atime_;
  statxbuf->stx_ctime.tv_sec = node.ctime_;
  statxbuf->stx_mtime.tv_sec = node.mtime_;
  return 0;
}

auto dfs_chmod(const char *pathname, mode_t mode) -> int {
  SPDLOG_TRACE("hook : dfs_chmod(pathname={},mode={:b})", pathname, mode);
  int ret = dfs::gPreloadCtx->rpc_client_->Chmod(pathname, mode);
  if (ret != 0) {
    errno = ENOENT;
  }
  return ret;
}

static auto cookie_read(void *cookie, char *buf, size_t size) -> ssize_t {
  // fprintf(stderr, "cookie_read called: fd=%d\n", *(int *)cookie);
  return dfs_read(*(int *)cookie, buf, size);
}

static auto cookie_write(void *cookie, const char *buf, size_t size)
    -> ssize_t {
  // fprintf(stderr, "cookie_write called: fd=%d\n", *(int *)cookie);
  return dfs_write(*(int *)cookie, buf, size);
}

static auto cookie_seek(void *cookie, off64_t *offset, int whence) -> int {
  // fprintf(stderr, "cookie_seek called: fd=%d\n", *(int *)cookie);
  int64_t ret = dfs_lseek(*(int *)cookie, *offset, whence);
  if (ret < 0) {
    return -1;
  }
  *offset = ret;
  return 0;
}

static auto cookie_close(void *cookie) -> int {
  // fprintf(stderr, "cookie_close called: fd=%d\n", *(int *)cookie);
  int fd = *(int *)cookie;
  free(cookie);
  auto &mtx = GetStreamFdMapMutex();
  std::lock_guard<std::mutex> lk(mtx);
  auto &map = GetStreamFdMap();
  auto it = map.begin();
  for (; it != map.end(); ++it) {
    if (fd == it->second) {
      break;
    }
  }
  if (it != map.end()) {
    map.erase(it);
  }
  return dfs_close(fd);
}

auto dfs_fdopen(int fd, const char *mode) -> FILE * {
  int *cookie = (int *)malloc(sizeof(int));
  if (cookie == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  *cookie = fd;

  cookie_io_functions_t iofuncs = {.read = cookie_read,
                                   .write = cookie_write,
                                   .seek = cookie_seek,
                                   .close = cookie_close};

  FILE *fp = fopencookie(cookie, mode, iofuncs);
  if (fp == nullptr) {
    free(cookie);
    return nullptr;
  }

  return fp;
}

auto dfs_fopen(const char *pathname, const char *mode) -> FILE * {
  dfs::Inode node;
  int ret = dfs::gPreloadCtx->rpc_client_->Stat(pathname, &node);
  if (ret != 0) { // file doesnt exist
    errno = ENOENT;
    return nullptr;
  }
  int fd = dfs::gPreloadCtx->fd_manager_->AllocateFd(node);
  FILE *fp = dfs_fdopen(fd, mode);
  if (fp == nullptr) {
    return nullptr;
  }

  auto &mtx = GetStreamFdMapMutex();
  std::lock_guard<std::mutex> lk(mtx);
  auto &map = GetStreamFdMap();
  map[fp] = fd;
  return fp;
}

auto dfs_fileno(FILE *stream) -> int {
  auto &mtx = GetStreamFdMapMutex();
  std::lock_guard<std::mutex> lk(mtx);
  auto &map = GetStreamFdMap();
  auto it = map.find(stream);
  if (it != map.end()) {
    return it->second;
  }
  return -1;
}

auto dfs_mmap(void *addr, size_t len, int prot, int flags, int fd,
              __off_t offset) -> void * {
  if (((flags & MAP_PRIVATE) != 0) && (prot & PROT_WRITE) == 0) {
    struct stat st;
    if (dfs_fstat(fd, &st) < 0) {
      errno = EBADF;
      return MAP_FAILED;
    }
    size_t file_sz = st.st_size;

    int64_t pagesz = sysconf(_SC_PAGESIZE);
    size_t map_len = ((len + pagesz - 1) / pagesz) * pagesz;

    void *mem = malloc(map_len);
    if (mem == nullptr) {
      errno = ENOMEM;
      return MAP_FAILED;
    }
    auto &mtx = GetMmapMapMutex();
    std::lock_guard<std::mutex> lk(mtx);
    auto &map = GetMmapMap();
    map[mem] = 1;

    ssize_t rd = dfs_pread(fd, mem, len, offset);
    if (rd < 0) {
      free(mem);
      errno = EIO;
      return MAP_FAILED;
    }

    if ((size_t)rd < map_len) {
      memset((char *)mem + rd, 0, map_len - rd);
    }
    return mem;
  }
  return nullptr;
}

auto dfs_munmap(void *addr, size_t len) -> int {
  auto &mtx = GetMmapMapMutex();
  std::lock_guard<std::mutex> lk(mtx);
  auto &map = GetMmapMap();
  auto it = map.find(addr);
  if (it != map.end()) {
    free(addr);
    map.erase(it);
    return 0;
  }
  return -1;
}

auto dfs_mkstemp(char *pathname) -> int {
  static const char letters[] = "abcdefghijklmnopqrstuvwxyz"
                                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "0123456789";
  size_t n = strlen(pathname);
  size_t num_x = 6;
  const size_t L = sizeof(letters) - 1;

  for (size_t i = 0; i < num_x; i++) {
    int64_t r = random();
    pathname[n - num_x + i] = letters[r % L];
  }

  return dfs_creat(pathname, 0644);
}

auto dfs_symlink(const char *linkname, const char *pathname) -> int {
  int fd = dfs_creat(pathname, 0777 | S_IFLNK);
  if (fd == -1) {
    return -1;
  }
  size_t len = strlen(linkname);
  int ret = dfs_write(fd, linkname, len);
  if (ret < 0 || (size_t)ret != len) {
    dfs_close(fd);
    return -1;
  }
  dfs_close(fd);
  return 0;
}

auto dfs_mknod(const char *pathname, mode_t mode, dev_t dev) -> int {
  SPDLOG_WARN("dfs_mknod hook unimplemented !");
  return 0;
}

auto dfs_statfs(const char *path, struct statfs *buf) -> int {
  // TODO(ubuntu):

  SPDLOG_WARN("dfs_statfs hook unimplemented !");
  return 0;
}

auto dfs_statvfs(const char *path, struct statvfs *buf) -> int {
  // TODO(ubuntu):
  SPDLOG_WARN("dfs_statvfs hook unimplemented !");
  return 0;
}
}
