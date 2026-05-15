#pragma once

#include "common/metadata_types.h"
#include "concurrentqueue.h"
#include "server/inode_array.h"
#include "server/level_hashtable_traits.h"
#include "server/rpc_server.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <rpc.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dfs {

inline auto NextDirent(Dirent *dents) -> Dirent * {
  return OffsetDirent(dents, dents->reclen_);
}

struct Result {
  // 0 OK, -1 fail, -EAGAIN checkpoint full need retry
  int mark_ = 0;
  uint64_t id_ = 0;
  std::string new_path_;

  Result() = default;

  explicit Result(int mark) : mark_(mark) {}

  explicit Result(int mark, uint64_t id) : mark_(mark), id_(id) {}

  explicit Result(int mark, uint64_t id, std::string new_path)
      : mark_(mark), id_(id), new_path_(std::move(new_path)) {}
};

struct OpenDirResult {
  Result res_;
  DirHandle handle_{};
  bool has_handle_ = false;

  OpenDirResult() = default;

  explicit OpenDirResult(Result res) : res_(std::move(res)) {}

  explicit OpenDirResult(Result res, DirHandle handle)
      : res_(std::move(res)), handle_(handle), has_handle_(true) {}
};

struct RenameResult {
  // 0 OK, -1 fail
  int mark_ = 0;
  std::string old_path_;
  std::string new_path_;

  RenameResult() = default;

  explicit RenameResult(int mark) : mark_(mark) {}

  explicit RenameResult(int mark, std::string old_path, std::string new_path)
      : mark_(mark), old_path_(std::move(old_path)),
        new_path_(std::move(new_path)) {}
};

struct GuardianContext;

// Transient staging for in-flight listdir operations. A `Listing` holds the
// full serialized DentView stream for one `(dir_id, cutoff_version)` pair so
// that when a listdir needs more than one GetDentViews RPC to transfer, the
// server can build the snapshot once and slice it across the follow-up RPCs
// instead of re-scanning DirPages / WAL and re-sorting on every call. This
// is NOT a directory cache: entries are only kept while the client is still
// draining them, and are released as soon as the terminating zero-byte RPC
// is served. A TTL sweep provides a safety net for crashed clients.
class PendingListings {
public:
  struct Listing {
    uint64_t dir_id{};
    uint64_t cutoff{};
    // Sorted + deduped DentView stream, already in the wire-compatible
    // record format (same bytes SerializeDentViews used to produce).
    std::vector<char> bytes;
    // Boundaries of each record in `bytes`: record_offsets[0] == 0 and
    // record_offsets.back() == bytes.size(). Used for O(log n) alignment
    // checks and chunk end cut.
    std::vector<size_t> record_offsets;
    std::chrono::steady_clock::time_point last_touch;
  };

  using ListingPtr = std::shared_ptr<Listing>;

  // Look up the listing for (dir_id, cutoff). On miss, call builder() outside
  // the table mutex to produce a fresh Listing, then publish it. Concurrent
  // misses on the same key each build independently; the last writer wins
  // and any losing snapshot still lives through its caller's shared_ptr.
  template <class Builder>
  auto GetOrStage(uint64_t dir_id, uint64_t cutoff,
                  Builder &&builder) -> ListingPtr;

  // Drop the staged listing for (dir_id, cutoff). Safe to call when no
  // entry is present (no-op). Other holders of the shared_ptr keep their
  // view alive until refcount hits zero.
  void Release(uint64_t dir_id, uint64_t cutoff);

  // Safety net: drop listings whose last_touch is older than `ttl`. Intended
  // to be called periodically from a housekeeping thread to prevent leaks
  // when a client dies mid-listdir.
  void SweepExpired(std::chrono::milliseconds ttl);

private:
  struct Key {
    uint64_t dir_id;
    uint64_t cutoff;
  };

  struct KeyHash {
    auto operator()(const Key &k) const noexcept -> size_t {
      // Mix both halves so (dir_id, cutoff) hash to distinct buckets even
      // when either half is dense.
      return std::hash<uint64_t>{}(k.dir_id) ^
             (std::hash<uint64_t>{}(k.cutoff) << 1);
    }
  };

  struct KeyEq {
    auto operator()(const Key &a, const Key &b) const noexcept -> bool {
      return a.dir_id == b.dir_id && a.cutoff == b.cutoff;
    }
  };

  std::mutex mu_;
  std::unordered_map<Key, ListingPtr, KeyHash, KeyEq> map_;
};

template <class Builder>
auto PendingListings::GetOrStage(uint64_t dir_id, uint64_t cutoff,
                                 Builder &&builder) -> ListingPtr {
  // Fast path: hit under the mutex, bump last_touch, return.
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(Key{dir_id, cutoff});
    if (it != map_.end()) {
      it->second->last_touch = std::chrono::steady_clock::now();
      return it->second;
    }
  }

  // Slow path: build outside the mutex so concurrent misses on other keys
  // don't serialize, then publish. Racing builders on the same key each
  // produce an independent snapshot; whichever one inserts last owns the
  // map slot, but every caller still returns its own fresh shared_ptr so
  // in-flight slices never see their bytes disappear underfoot.
  ListingPtr fresh = builder();
  if (!fresh) {
    return nullptr;
  }
  fresh->dir_id = dir_id;
  fresh->cutoff = cutoff;
  fresh->last_touch = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(mu_);
    map_[Key{dir_id, cutoff}] = fresh;
  }
  return fresh;
}

// This class handles all metadata storage methods, i.e., put/get/delete
class Metadata {
  Metadata() = default;

  // Every id after min_free_id is unused. Every id before min_avail_id is used.
  moodycamel::ConcurrentQueue<uint64_t> recycled_ids_;
  std::atomic<uint64_t> min_unused_id_, min_avail_id_;
  uint64_t base_id_;
  uint64_t max_id_;

  // Dirent: Level Hash Table
  DirentHashtable *dhashtable_; // dirent hashtable
  // Resize shared meta (CXL offsets)
  uint64_t d_resize_meta_off_ = 0;

#ifdef USING_INODE_ARRAY
  // Inode: Direct-mapped array on CXL
  InodeArray *ihashtable_; // inode array
#else
  // Inode: Level Hash Table
  InodeHashtable *ihashtable_;
  uint64_t i_resize_meta_off_ = 0;
#endif

#ifdef HASHTABLE_LATENCY_PROFILE
  std::atomic<uint64_t> dhashtable_times_ = 0, ihashtable_times_ = 0,
                        dhashtable_time_ = 0, ihashtable_time_ = 0;
#endif

  // buffer pool
  int buffer_size_ = DIR_MAX_ALLOC;
  int pool_size_ = 10;
  std::vector<std::unique_ptr<char[]>> buffers_;
  std::atomic_bool in_use_[10];

  // In-flight listdir staging: first GetDentViews for a (dir_id, cutoff)
  // builds the full serialized stream and parks it here; follow-up paged
  // RPCs slice from it in O(log n); the terminating zero-byte RPC releases.
  PendingListings pending_listings_;

  void CollectPersistentDentViews(
      uint64_t inode_id, std::vector<DentView> &entries,
      uint64_t read_cutoff_version = std::numeric_limits<uint64_t>::max());
  void CollectPersistentDirents(uint64_t inode_id,
                                std::vector<Dirent> &entries);
  auto SerializeDirents(const std::vector<Dirent> &entries,
                        Dirent *result_buffer, uint32_t size,
                        int offset) -> uint64_t;

public:
  int meta_num_;
  int all_meta_num_;
  static auto Init(std::string_view meta_root,
                   int meta_num = 0) -> std::unique_ptr<Metadata>;

  void InitHashtable();

  void SetMetaNum(int all_meta_num);

  ~Metadata();

  auto AcquireBuffer() -> char *;

  void ReleaseBuffer(char *buffer);

  void PrintSpace();

  // ############## Level 1: Basic function: GET/PUT/DELETE
  // #################

  // Retrieves an inode from storage.
  // @param inode_id The id of inode you wish to find.
  // @param result_buffer Where to put the inode if it's found.
  // @return true if inode found, false otherwise.
  auto GetInode(uint64_t inode_id, Inode *result_buffer) -> bool;

  // Stores an inode into storage.
  // @param inode_id The id of inode you wish to store.
  // @param source Pointer to the inode to be stored
  void PutInode(uint64_t inode_id, Inode *source);

  // Update an inode
  void UpdateInode(uint64_t inode_id, Inode *source);

  // Remove an inode from hashtable and storage.
  // @param inode_id The id of inode you wish to remove.
  void DeleteInode(uint64_t inode_id);

  // Retrieves a directory table from storage.
  // @param inode_id The DIRECTORY's inode_id you wish to find.
  // @param result_buffer Where to put the table if it's found.
  // @param size Buffer size in bytes.
  // @param offset Where to start reading.
  // @return number of bytes read. 0 indicates EOF or error.
  auto GetDents(uint64_t inode_id, Dirent *result_buffer, uint32_t size,
                int offset) -> uint64_t;

  auto GetDentViews(uint64_t inode_id, uint64_t read_cutoff_version,
                    DentView *result_buffer, uint32_t size,
                    int offset) -> uint64_t;

  // ############## Level 2: Useful (intermediate) utilities
  // #################

  // Path walk function: find path's inode_id
  // @param path The ABSOLUTE PATH of the file/directory you wish to find.
  // @param parent_buf INPUT: If given a non-null ptr and a non-zero value
  // x, I will start searching from the directory whose id is x. OUTPUT: If
  // PARENT is found, I will put its parent's id here, otherwise I will make
  // sure you get a 0.
  // @param ent_ptr When given a non-null buffer, we will put the entry if
  // it's found, otherwise we put the parsed name if its parent is found
  // here.
  // @param Length of path to be parsed. Default = -1 = to the end of it.
  // @return 0, inode_id if the path exists, -1, 0 if not found
  // new_path if subsequent paths in next meta.
  auto LocateInode(const char *path, uint64_t *parent_buf = nullptr,
                   Dirent *ent_ptr = nullptr, int path_len = -1) -> Result;

  void AddDirectoryEntry(Dirent *entry, uint64_t parent_id,
                         bool update_parent = true);

  void DeleteDirectoryEntry(Dirent *entry, uint64_t parent_id,
                            bool update_parent = true);

  void FreeInodeId(uint64_t id);

  auto AllocateInodeId() -> uint64_t;

  // ############## Level 3: Syscall Wrapper / Helper #################

  // stat(path, buffer) -> 0, OK | -1, fail
  // Subsequent paths in next_meta
  inline auto Stat(const char *path, Inode *buffer) -> Result {
    Result res = LocateInode(path);
    if (res.mark_ == -1) {
      return res;
    }
    return Result(static_cast<int>(GetInode(res.id_, buffer)) - 1);
  };

  // unlink(path) -> 0, OK | -1, fail
  auto Unlink(const char *path, bool is_dir = false) -> Result;

  // rmdir(path) -> 0, OK | -1, fail
  inline auto Rmdir(const char *path) -> Result { return Unlink(path, true); }

  // access(path, mode) -> 0, OK | -1, fail
  auto Access(const char *path, uint32_t mode, uint32_t uid,
              uint32_t gid) -> Result;

  // chmod(path, mode) -> 0, OK | -1, fail
  auto Chmod(const char *path, uint32_t mode, uint32_t uid,
             uint32_t gid) -> Result;

  // create function ->  0, OK | -1, fail
  // @WARNING: Passing buf = NULL will turn it into mkdir()!
  auto Create(const char *path, uint32_t mode, uint32_t uid, uint32_t gid,
              Inode *buf) -> Result;

  // mkdir(path, mode) ->  0, OK | -1, fail
  inline auto Mkdir(const char *path, uint32_t mode, uint32_t uid,
                    uint32_t gid) -> Result {
    return Create(path, mode, uid, gid, nullptr);
  }

  // rename(oldpath, newpath) -> 0, OK | -1, fail
  auto Rename(const char *oldpath, const char *newpath,
              bool link = false) -> int;

  // link(oldpath, newpath) -> 0, OK | -1, fail
  inline auto Link(const char *oldpath, const char *newpath) -> int {
    return Rename(oldpath, newpath, true);
  }

  // opendir(path) -> DIR *dptr | nullptr
  inline auto OpenDir(const char *path,
                      int alloc = DIR_MAX_ALLOC) -> OpenDirResult {
    Result res = LocateInode(path);
    DirHandle handle;
    if (res.id_ == 0) {
      return OpenDirResult(res, handle);
    }
    handle.id_ = res.id_;
    return OpenDirResult(res, handle);
  }

  // fsync(fd) -> 0 | -1
  auto MetaSync(uint64_t id) -> int;
};

} // namespace dfs
