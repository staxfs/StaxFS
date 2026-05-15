#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#define USING_CXL_OFFSET // Enable CXL memory.

#define USING_LEVEL_HASHTABLE

// #define USING_LEVEL_HASHTABLE_BASELINE
// Use the Level-Hashing paper-faithful baseline (CXL-backed, stop-the-world
// resize, per-slot CAS spinlocks, separate 64 B occupancy word, 4 slots/bucket.
// Benchmark use only. Toggle by uncommenting.

#define HASHTABLE_BUCKET_NUM                                                   \
  1ULL << 18 // hashtable size = HASHTABLE_BUCKET_NUM * 8 * 1.5

#define LEVEL_HASHTABLE_TAG_HINT

// #define LEVEL_HASHTABLE_STATS
// Enable TwoLevelHashtable find()-path counters for measuring TAG_HINT
// effectiveness. Use scripts/analyze_hint.py analysis statistics results.

#define USING_INODE_ARRAY

// #define LISTDIR_LATENCY_PROFILE
// Enable per-step latency instrumentation for the listdir hot path on both
// client (dfs_opendir / LoadMergedDirents) and server (MDFDCommonReqHandler
// / CollectPersistentDentViews). See include/common/listdir_profile.h.

// #define MD_OP_LATENCY_PROFILE
// Enable per-op latency stats (count/avg/p50/p99/p999/p9999/max) for the
// 8 metadata ops handled by MDPathCommonReqHandler. See
// include/common/metadata_op_profile.h. Use scripts/analyze_md_op.py analysis
// statistics results.

// #define HASHTABLE_LATENCY_PROFILE

// #define CHECKPOINT_STATS_PROFILE
// Enable per-checkpoint sync telemetry in CXLPersistence: clock-based
// timing of inode/dent flush phases, page/byte counters, and a rolling-
// window log line every 32 syncs.

// #define CACHE_SKIP // Use cache skip strategy. Unused.
#define USING_CXL_PERSISTENCE

#define DIR_MAX_ALLOC 1048576 // 1MB
#define HUGEPAGES 1024
// Used for erpc, modify based on the maximum number of meta(data) threads *
// number of clients
#define INODE_ID_RANGE 50 // mata0: [0, 2^50), mate1: [2^50+1, 2^51), ...

#define CXL_CAPACITY 20  // Allocate CXL memory (GB)
#define GIM_CAPACITY 128 // Allocate GIM memory (MB)
#define CXL_NUMA_NODE 4  // Allocate CXL memory on NUMA node
#define CXLSSD_CAPACITY_MB                                                     \
  1024 // CXL-SSD DRAM total capacity (MB), used for Wal
#define CXLSSD_INODE_REGION_GB 16      // per-meta inode log region size (GB)
#define CXLSSD_DENT_REGION_GB 16       // per-meta dirent log region size (GB)
#define CXLSSD_NUMA_NODE CXL_NUMA_NODE // NUMA node for CXL-SSD DRAM
#define CXLSSD_CHECKPOINT_INTERVAL_MS                                          \
  1 // Idle sleep when checkpoint thread has no work (ms)
#define CXLSSD_WAL_ENTRIES (1ULL << 21) // 2M entries = 128MB per-MDS WAL
#define CXLSSD_REMOTE_INODE_WAL_ENTRIES                                        \
  (1ULL << 21) // 2M entries = 128MB per-MDS remote inode WAL

#define CXL_PATH "/dev/hugepages/cxl_memory"
#define GIM_PATH "/dev/hugepages/gim_memory"
#define CXLSSD_PATH "/sharenvme/usershome/hyx/dfs_ssd"

struct Dirent;

namespace dfs {

constexpr int kMaxFilenameLen = 63;
constexpr int kRootId = 2;

enum RemoteInodeChangeOp : uint8_t {
  kRemoteInodeNoop = 0,
  kRemoteInodeUnlink = 1,
  kRemoteInodeLink = 2,
  kRemoteInodeSetattr = 3,
  kRemoteInodeTouchCtime = 4,
  kRemoteParentBlocksDelta = 5,
};

struct RemoteInodeChange {
  RemoteInodeChangeOp op_ = kRemoteInodeNoop;
  uint64_t inode_id_ = 0;
  int64_t value_ = 0;
  uint64_t version_ = 0;
};

struct ObjectLocation {
  uint64_t node_id_ : 48;
  uint64_t disk_id_ : 16;
};

union ObjectUuid {
  ObjectLocation location_;
  uint64_t reserved_;
};

struct Inode {
  // ^ Filename is stored in dir_entry's, not in inode's
  uint64_t id_ = 0; // file serial number
  nlink_t nlink_;  // link count telling how many hard links point to the inode.
  mode_t mode_;    // file type and how the file's owner, its group,
                   // and others can access the file.
  uint32_t extra_; // explicit padding, not to be used outside metadata section.
                   // We use it for counting subdirectories and subfiles
  uint32_t uid_;   // owner's User ID, u64 used for padding.
  gid_t gid_;      // group ID
  dev_t rdev_;     // Reserved to make POSIX happy, at least for now
  size_t size_;    // size of file in bytes
  size_t blksize_; // preferred IO block size (of this file itself)
  size_t blocks_;  // number of blocks allocated to this file
  time_t atime_;   // access time
  time_t mtime_;   // modification time
  time_t ctime_;   // inode change time

  // ObjectLocation locations_[5]; // where children objects are
  // objposlist only has 5 entries, worry not, that's just for ease of debug.
  // later we will extend it to necessary length

  Inode(uint64_t id, nlink_t nlink, mode_t mode, uint32_t uid, gid_t gid,
        time_t atime, time_t mtime, time_t ctime, size_t size = 0,
        size_t blocks = 0)
      : id_(id), nlink_(nlink), mode_(mode), uid_(uid), gid_(gid),
        atime_(atime), mtime_(mtime), ctime_(ctime), size_(size),
        blocks_(blocks), extra_(0), rdev_(0), blksize_(0) {}

  Inode() = default;

  static auto Create(uint64_t id, nlink_t nlink, mode_t mode, uint32_t uid,
                     gid_t gid, time_t atime, time_t mtime, time_t ctime,
                     size_t size = 0, size_t blocks = 0) -> Inode * {
    auto *new_inode = new Inode();
    new_inode->id_ = id;
    new_inode->nlink_ = nlink;
    new_inode->mode_ = mode;
    new_inode->uid_ = uid;
    new_inode->gid_ = gid;
    new_inode->size_ = size;
    new_inode->blocks_ = blocks;
    new_inode->atime_ = atime;
    new_inode->mtime_ = mtime;
    new_inode->ctime_ = ctime;
    new_inode->extra_ = 0;
    new_inode->rdev_ = 0;
    new_inode->blksize_ = 0;
    return new_inode;
  }

  static void Delete(Inode *inode) { delete inode; }

  void Clear() { id_ = 0; }
};

template <typename DentType>
inline auto OffsetDirent(DentType *dents, int offset) -> DentType * {
  return reinterpret_cast<DentType *>(reinterpret_cast<char *>(dents) + offset);
}

} // namespace dfs

struct Dirent {
  ino_t id_ = 0;
  ino_t pid_;

  uint16_t reclen_;
  unsigned char type_;
  char name_[64] = {};

  Dirent(uint64_t id, uint64_t pid, uint16_t type, const char *name)
      : id_(id), pid_(pid), type_(type) {
    strcpy(name_, name);
    SetRecLen();
  }

  Dirent() = default;

  void SetRecLen() { reclen_ = offsetof(Dirent, name_) + strlen(name_) + 1; }

  void ToLinuxDirent(dirent &d) {
    d.d_ino = id_;
    d.d_off = 0;
    d.d_reclen = reclen_;
    d.d_type = type_;
    strcpy(d.d_name, name_);
  }

  static auto Create(ino_t id, ino_t pid, unsigned char type,
                     const char *name) -> Dirent * {
    auto *new_dirent = new Dirent();
    new_dirent->id_ = id;
    new_dirent->pid_ = pid;
    new_dirent->type_ = type;
    strcpy(new_dirent->name_, name);
    new_dirent->reclen_ =
        offsetof(Dirent, name_) + strlen(new_dirent->name_) + 1;
    return new_dirent;
  }

  static void Delete(Dirent *dirent) { delete dirent; }

  void Clear() { id_ = 0; }
};

struct DirHandle {
  uint64_t id_ = 0;
  uint64_t read_cutoff_version_ = 0;
};

struct DentView {
  static constexpr uint8_t kFlagTombstone = 0x01;

  ino_t id_ = 0;
  ino_t pid_ = 0;
  uint64_t version_ = 0;

  uint16_t reclen_ = 0;
  unsigned char type_ = DT_UNKNOWN;
  uint8_t flags_ = 0;
  char name_[64] = {};

  DentView(uint64_t id, uint64_t pid, uint64_t version, uint16_t type,
           const char *name, uint8_t flags = 0)
      : id_(id), pid_(pid), version_(version), type_(type), flags_(flags) {
    strcpy(name_, name);
    SetRecLen();
  }

  DentView() = default;

  void SetRecLen() { reclen_ = offsetof(DentView, name_) + strlen(name_) + 1; }

  auto IsDeleted() const -> bool { return (flags_ & kFlagTombstone) != 0; }
};
