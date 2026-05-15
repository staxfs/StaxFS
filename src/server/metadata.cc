
#include "server/metadata.h"

#include "common/listdir_profile.h"
#include "common/metadata_op_profile.h"
#include "common/metadata_types.h"
#include "cxl/cxl_mem.h"
#include "cxl/device.h"
#include "fmt/format.h"
#include "libcuckoo/cuckoohash_util.hh"
#include "server/cxl_persistence.h"
#include "server/hashtable_stats.h"
#include "server/rpc_server.h"
#include "server/s2fifo_cache.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using libcuckoo::UpsertContext;
using std::string;
using std::string_view;

namespace dfs {

namespace {

auto InodeModeToDirentType(mode_t mode) -> unsigned char {
  if (S_ISDIR(mode))
    return DT_DIR;
  if (S_ISREG(mode))
    return DT_REG;
  if (S_ISLNK(mode))
    return DT_LNK;
  if (S_ISCHR(mode))
    return DT_CHR;
  if (S_ISBLK(mode))
    return DT_BLK;
  if (S_ISFIFO(mode))
    return DT_FIFO;
  if (S_ISSOCK(mode))
    return DT_SOCK;
  return DT_UNKNOWN;
}

auto ExtractWalName(const CompactLogEntry *entry, const CompactWAL &wal,
                    uint64_t pos) -> std::string {
  std::string name(entry->name_len_, '\0');
  int inline_len = std::min<int>(entry->name_len_, CompactLogEntry::kInlineMax);
  std::memcpy(name.data(), entry->name_, inline_len);
  int remaining = entry->name_len_ - inline_len;
  for (uint8_t i = 0; i < entry->ContCount() && remaining > 0; ++i) {
    auto *ov = wal.OverflowAt(pos + 1 + i);
    int copy_len = std::min<int>(remaining, CompactLogOverflow::kDataMax);
    std::memcpy(name.data() + inline_len + i * CompactLogOverflow::kDataMax,
                ov->data_, copy_len);
    remaining -= copy_len;
  }
  return name;
}

auto ExtractWalRenameNames(const CompactLogEntry *entry, const CompactWAL &wal,
                           uint64_t pos)
    -> std::pair<std::string, std::string> {
  int old_len = entry->name_len_;
  int new_len = entry->aux_len_;
  int total = old_len + new_len;
  std::string full(total, '\0');
  int inline_total = std::min(total, CompactLogEntry::kInlineMax);
  std::memcpy(full.data(), entry->name_, inline_total);
  int remaining = total - inline_total;
  for (uint8_t i = 0; i < entry->ContCount() && remaining > 0; ++i) {
    auto *ov = wal.OverflowAt(pos + 1 + i);
    int copy_len = std::min<int>(remaining, CompactLogOverflow::kDataMax);
    std::memcpy(full.data() + inline_total + i * CompactLogOverflow::kDataMax,
                ov->data_, copy_len);
    remaining -= copy_len;
  }
  return {full.substr(0, old_len), full.substr(old_len, new_len)};
}

} // namespace

auto Metadata::Init(string_view meta_root,
                    int meta_num) -> std::unique_ptr<Metadata> {
  SPDLOG_INFO("Init meta. meta id = {}", meta_num);
  auto meta = std::unique_ptr<Metadata>(new Metadata);
  meta->meta_num_ = meta_num;
  meta->all_meta_num_ = meta_num + 1;
  meta->base_id_ = (static_cast<uint64_t>(meta_num) << INODE_ID_RANGE);
  meta->max_id_ = (static_cast<uint64_t>(meta_num + 1) << INODE_ID_RANGE);
  meta->InitHashtable();
  // Inode and directory persistence is handled by CXL WAL + SSD regions.
  // No legacy log files needed.
  std::filesystem::create_directories(std::filesystem::path(meta_root));
  // meta->min_avail_id_ = meta->min_unused_id_ = meta->base_id_ + kRootId + 1;
  meta->min_unused_id_ = meta->base_id_ + kRootId + 1;
  // * Prepare root directory "/"
  if (meta_num == 0) {
    time_t now = time(nullptr);
    Inode root_inode(kRootId, 1, 0755 | S_IFDIR, 1, 2, now, now, now);
    meta->PutInode(kRootId, &root_inode);
  }
  meta->buffers_.reserve(meta->pool_size_);
  for (size_t i = 0; i < meta->pool_size_; ++i) {
    meta->buffers_.emplace_back(std::make_unique<char[]>(meta->buffer_size_));
    meta->in_use_[i].store(false);
  }

  return meta;
}

void Metadata::InitHashtable() {
  constexpr size_t kDTLBuckets = HASHTABLE_BUCKET_NUM;
  auto *dfs_hdr = gDevice->dfs_header_;

  if (meta_num_ == 0) {
    d_resize_meta_off_ = gDevice->CXLMemMalloc(sizeof(ResizeMeta));
    dhashtable_ = new DirentHashtable(kDTLBuckets, all_meta_num_);
    dfs_hdr->d_resize_meta_offset_ = d_resize_meta_off_;
    ResizeMeta d_resize = {0,
                           0,
                           dhashtable_->tl_offset(),
                           kDTLBuckets,
                           dhashtable_->bl_offset(),
                           kDTLBuckets / 2,
                           0,
                           0};
    gDevice->CXLWriteSync(d_resize_meta_off_, sizeof(ResizeMeta), &d_resize);
    dfs_hdr->magic_num_.store(0x43584C53, std::memory_order_release);
  } else {
    while (!dfs_hdr->IsInit()) {
    }
    d_resize_meta_off_ = dfs_hdr->d_resize_meta_offset_;
    ResizeMeta d_resize;
    gDevice->CXLReadSync(d_resize_meta_off_, sizeof(ResizeMeta), &d_resize);
    dhashtable_ = new DirentHashtable(kDTLBuckets, d_resize.tl_offset,
                                      d_resize.bl_offset, all_meta_num_);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300 * meta_num_));
  auto *d_notify = static_cast<dfs::GIMResizeNotify *>(
      gDevice->GIMMemMallocPointer(sizeof(dfs::GIMResizeNotify)));
  new (d_notify) dfs::GIMResizeNotify();
  dfs_hdr->d_resize_notify_offset_[meta_num_] =
      gDevice->GetGIMMemOffset(d_notify);

  dhashtable_->enable_resize({d_resize_meta_off_, meta_num_, all_meta_num_,
                              dfs_hdr->d_resize_notify_offset_});

#ifdef USING_INODE_ARRAY
  ihashtable_ = new InodeArray(meta_num_, all_meta_num_, base_id_);
#else
  // ── Inode hashtable: shared TwoLevelHashtable (same as dirent) ──
  constexpr size_t kITLBuckets = HASHTABLE_BUCKET_NUM;
  if (meta_num_ == 0) {
    i_resize_meta_off_ = gDevice->CXLMemMalloc(sizeof(ResizeMeta));
    ihashtable_ = new InodeHashtable(kITLBuckets, all_meta_num_);
    dfs_hdr->i_resize_meta_offset_ = i_resize_meta_off_;
    ResizeMeta i_resize = {0,
                           0,
                           ihashtable_->tl_offset(),
                           kITLBuckets,
                           ihashtable_->bl_offset(),
                           kITLBuckets / 2,
                           0,
                           0};
    gDevice->CXLWriteSync(i_resize_meta_off_, sizeof(ResizeMeta), &i_resize);
  } else {
    i_resize_meta_off_ = dfs_hdr->i_resize_meta_offset_;
    ResizeMeta i_resize;
    gDevice->CXLReadSync(i_resize_meta_off_, sizeof(ResizeMeta), &i_resize);
    ihashtable_ = new InodeHashtable(kITLBuckets, i_resize.tl_offset,
                                     i_resize.bl_offset, all_meta_num_);
  }
  auto *i_notify = static_cast<dfs::GIMResizeNotify *>(
      gDevice->GIMMemMallocPointer(sizeof(dfs::GIMResizeNotify)));
  new (i_notify) dfs::GIMResizeNotify();
  dfs_hdr->i_resize_notify_offset_[meta_num_] =
      gDevice->GetGIMMemOffset(i_notify);
  ihashtable_->enable_resize({i_resize_meta_off_, meta_num_, all_meta_num_,
                              dfs_hdr->i_resize_notify_offset_});
#endif
}

void Metadata::SetMetaNum(int all_meta_num) {
  all_meta_num_ = all_meta_num;
  dhashtable_->SetMds(all_meta_num,
                      gDevice->dfs_header_->d_resize_notify_offset_);
#ifdef USING_INODE_ARRAY
  ihashtable_->SetMds(all_meta_num);
#else
  ihashtable_->SetMds(all_meta_num,
                      gDevice->dfs_header_->i_resize_notify_offset_);
#endif
}

Metadata::~Metadata() {
  delete dhashtable_;
  delete ihashtable_;
}

static auto GetMemSize() -> size_t {
  FILE *file = fopen("/proc/self/status", "r");
  size_t result = -1;
  char line[128];

  while (fgets(line, 128, file) != nullptr) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      int len = strlen(line);

      const char *p = line;
      for (; !static_cast<bool>(std::isdigit(*p)); ++p) {
      }

      line[len - 3] = 0;
      result = atoll(p);

      break;
    }
  }
  fclose(file);
  return result;
}

auto Metadata::AcquireBuffer() -> char * {
  while (true) {
    for (size_t i = 0; i < pool_size_; ++i) {
      bool expected = false;
      if (in_use_[i].compare_exchange_strong(expected, true,
                                             std::memory_order_acquire)) {
        // memset(buffers_[i].get(), 0, buffer_size_);
        return buffers_[i].get();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return nullptr;
}

void Metadata::ReleaseBuffer(char *buffer) {
  for (size_t i = 0; i < pool_size_; ++i) {
    if (buffers_[i].get() == buffer) {
      in_use_[i].store(false, std::memory_order_release);
      return;
    }
  }
}

static auto GetMemSizeForThread(const std::string &path) -> size_t {
  FILE *file = fopen(path.c_str(), "r");
  size_t result = -1;
  char line[128];

  if (file == nullptr) {
    return result;
  }

  while (fgets(line, 128, file) != nullptr) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      const char *p = line;
      while (std::isdigit(*p) == 0) {
        ++p;
      }
      result = atoll(p);
      break;
    }
  }
  fclose(file);
  return result;
}

static auto GetTotalMemSize() -> size_t {
  size_t total_memory = 0;

  total_memory += GetMemSizeForThread("/proc/self/status");

  DIR *dir = opendir("/proc/self/task");
  if (dir == nullptr) {
    perror("Cannot open /proc/self/task");
    return total_memory;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_type == DT_DIR &&
        std::all_of(entry->d_name, entry->d_name + std::strlen(entry->d_name),
                    ::isdigit)) {
      std::string thread_path =
          std::string("/proc/self/task/") + entry->d_name + "/status";
      total_memory += GetMemSizeForThread(thread_path);
    }
  }

  closedir(dir);
  return total_memory;
}

void Metadata::PrintSpace() {
  auto format_size = [](size_t size) -> std::string {
    static const char *sizes[] = {"B", "KB", "MB", "GB", "TB"};
    int order = 0;
    double dbl_size = size;

    while (dbl_size >= 1024 && order < (sizeof(sizes) / sizeof(sizes[0])) - 1) {
      order++;
      dbl_size /= 1024;
    }

    char formatted_size[20];
    snprintf(formatted_size, sizeof(formatted_size), "%.2f %s", dbl_size,
             sizes[order]);
    return {formatted_size};
  };

  auto format_size2 = [](size_t size) -> std::string {
    static const char *sizes[] = {"", "10^3", "10^6", "10^9", "10^12"};
    int order = 0;
    double dbl_size = size;

    while (dbl_size >= 1000 && order < (sizeof(sizes) / sizeof(sizes[0])) - 1) {
      order++;
      dbl_size /= 1000;
    }

    char formatted_size[20];
    snprintf(formatted_size, sizeof(formatted_size), "%.2f %s", dbl_size,
             sizes[order]);
    return {formatted_size};
  };

  // Drop any pending listdir stagings whose client went away mid-drain.
  // Normal flow releases them on the zero-byte terminator RPC; this sweep
  // is just a leak guard for crashed or timed-out readers.
  pending_listings_.SweepExpired(std::chrono::seconds(30));

  SPDLOG_INFO("dhashtable total space = {}",
              format_size(dhashtable_->GetSpace()));
  SPDLOG_INFO("ihashtable total space = {}",
              format_size(ihashtable_->GetSpace()));

#ifdef HASHTABLE_LATENCY_PROFILE
  SPDLOG_INFO("dhashtable times = {}", format_size2(dhashtable_times_.load()));
  if (dhashtable_times_ != 0) {
    SPDLOG_INFO("dhashtable_operate_avg_time_ = {}",
                format_size2(dhashtable_time_ / dhashtable_times_));
  } else {
    SPDLOG_INFO("dhashtable_operate_avg_time_ = 0");
  }
  dhashtable_times_.store(0);
  dhashtable_time_.store(0);

  SPDLOG_INFO("ihashtable times = {}", format_size2(ihashtable_times_.load()));
  if (ihashtable_times_ != 0) {
    SPDLOG_INFO("ihashtable_operate_avg_time_ = {}",
                format_size2(ihashtable_time_ / ihashtable_times_));
  } else {
    SPDLOG_INFO("ihashtable_operate_avg_time_ = 0");
  }
  ihashtable_times_.store(0);
  ihashtable_time_.store(0);
#endif

  SPDLOG_INFO("CXL Memory malloc size = {}, remaining space = {}",
              format_size(gDevice->cxl->Allocator()->offset_.load()),
              format_size(gDevice->cxl->Allocator()->capacity_ -
                          gDevice->cxl->Allocator()->offset_.load()));

  SPDLOG_INFO("mem_size = {}", format_size(GetMemSize() * 1024));
  SPDLOG_INFO("total_size = {}", format_size(GetTotalMemSize() * 1024));
#ifdef LEVEL_HASHTABLE_STATS
  dfs::HashtableStatsDump();
  dfs::HashtableStatsReset();
#endif
  LISTDIR_PROFILE_DUMP("server-periodic");
  MD_OP_PROFILE_DUMP("server-periodic");
}

auto Metadata::GetInode(const uint64_t inode_id, Inode *result_buffer) -> bool {
  DTRACE_PROBE1(DFS_META_SERVER, GetInode_enter, inode_id);
  if (inode_id == 0) {
    SPDLOG_ERROR("GetInode failed. inode_id = 0");
    return false;
  }
  bool ret;
#ifdef HASHTABLE_LATENCY_PROFILE
  uint64_t st = Gethrtime();
  ihashtable_times_.fetch_add(1);
#endif
  ret = ihashtable_->find(inode_id, *result_buffer);
#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_time_.fetch_add(Gethrtime() - st);
#endif
  if (ret) {
    DTRACE_PROBE1(DFS_META_SERVER, GetInode_hashtable_hit_ret, inode_id);
    return true;
  }

  SPDLOG_ERROR("GetInode shouldn't run here temporarily");
  DTRACE_PROBE1(DFS_META_SERVER, GetInode_hashtable_miss_ret, inode_id);
  return false;
}

void Metadata::PutInode(uint64_t inode_id, Inode *source) {
#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_times_.fetch_add(1);
  uint64_t st = Gethrtime();
#endif
  if (ihashtable_->insert(inode_id, *source) != 1) {
    SPDLOG_ERROR("PutInode: ihashtable insert failed. id = {}", inode_id);
  }
#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_time_.fetch_add(Gethrtime() - st);
#endif
}

void Metadata::UpdateInode(uint64_t inode_id, Inode *source) {
  if (!ihashtable_->update_fn(inode_id,
                              [source](Inode &cur) { cur = *source; })) {
    SPDLOG_ERROR("UpdateInode failed. inode_id = {}", inode_id);
  }
}

void Metadata::DeleteInode(const uint64_t inode_id) {
#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_times_.fetch_add(1);
  uint64_t st = Gethrtime();
#endif
  ihashtable_->erase(inode_id);
#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_time_.fetch_add(Gethrtime() - st);
#endif
}

void Metadata::FreeInodeId(uint64_t id) {
  if (id >= base_id_ && id < max_id_) {
    uint64_t expected = id + 1;
    if (!min_unused_id_.compare_exchange_strong(expected, id)) {
      recycled_ids_.enqueue(id);
    }
  }
  if (min_avail_id_ > id) {
    min_avail_id_ = id;
  }
}

auto Metadata::AllocateInodeId() -> uint64_t {
  uint64_t id;
  if (min_avail_id_ == min_unused_id_ || !recycled_ids_.try_dequeue(id)) {
    id = min_unused_id_.fetch_add(1);
    min_avail_id_ = id + 1;
  }
  return id;
}

void Metadata::CollectPersistentDentViews(uint64_t inode_id,
                                          std::vector<DentView> &entries,
                                          uint64_t read_cutoff_version) {
  LISTDIR_PROFILE_SCOPE(dfs::kSrvCollectDentViews);
  entries.clear();
  if (gCXLPersistence == nullptr) {
    return;
  }

  // Snapshot WAL boundaries BEFORE reading DentRegion to avoid a race where
  // checkpoint writes entries to DentRegion, clears WAL, and advances
  // checkpoint_pos_ between our DentRegion read and WAL scan.  By capturing
  // checkpoint_pos_ first, our WAL scan covers any entries that may not yet
  // have been flushed to DentRegion when we read it.
  auto *wal = gCXLPersistence->WAL();
  uint64_t pos = wal->CheckpointPos();
  uint64_t head = wal->Head();

  // Flat scratch vector — every DirPage record and every applicable WAL tail
  // entry is appended as a raw DentView. No hash maps, no std::string heap
  // allocations. Sort + linear dedup happens once at the end.
  std::vector<DentView> collected;

  {
    LISTDIR_PROFILE_SCOPE(dfs::kSrvReadDirLatest);
    gCXLPersistence->DentRegion()->ReadDirLatestFlat(
        inode_id, read_cutoff_version, collected);
  }

  // Helper: stamp a DentView from an op+name and append to `collected`.
  // WAL tail is typically empty in steady state (checkpoint thread keeps up),
  // so allocations inside this lambda are rarely hit.
  auto push_view = [&](uint64_t id, uint8_t type, bool deleted,
                       uint64_t version, const std::string &name) {
    collected.emplace_back();
    auto &view = collected.back();
    view.id_ = id;
    view.pid_ = inode_id;
    view.version_ = version;
    view.type_ = type;
    view.flags_ = deleted ? DentView::kFlagTombstone : 0;
    size_t copy_len = std::min<size_t>(name.size(), sizeof(view.name_) - 1);
    std::memcpy(view.name_, name.data(), copy_len);
    view.name_[copy_len] = '\0';
    view.SetRecLen();
  };

  {
    LISTDIR_PROFILE_SCOPE(dfs::kSrvWalTailScan);
    while (pos < head) {
      const auto *entry = wal->At(pos);
      if (!entry->IsReady()) {
        break;
      }

      switch (entry->op_) {
      case kWALCreate: {
        if (entry->version_ <= read_cutoff_version &&
            entry->parent_id_ == inode_id) {
          auto name = ExtractWalName(entry, *wal, pos);
          push_view(entry->inode_id_, entry->aux_len_, false, entry->version_,
                    name);
        }
        break;
      }
      case kWALUnlink: {
        if (entry->version_ <= read_cutoff_version &&
            entry->parent_id_ == inode_id) {
          auto name = ExtractWalName(entry, *wal, pos);
          push_view(entry->inode_id_, DT_UNKNOWN, true, entry->version_, name);
        }
        break;
      }
      case kWALRename: {
        uint64_t new_parent = entry->GetNewParent();
        bool old_match = entry->version_ <= read_cutoff_version &&
                         entry->parent_id_ == inode_id;
        bool new_match =
            entry->version_ <= read_cutoff_version && new_parent == inode_id;
        if (old_match || new_match) {
          auto [old_name, new_name] = ExtractWalRenameNames(entry, *wal, pos);
          if (old_match) {
            push_view(entry->inode_id_, DT_UNKNOWN, true, entry->version_,
                      old_name);
          }
          if (new_match) {
            push_view(entry->inode_id_, entry->GetDirentType(), false,
                      entry->version_, new_name);
          }
        }
        break;
      }
      case kWALLink: {
        uint64_t new_parent = entry->GetNewParent();
        if (entry->version_ <= read_cutoff_version && new_parent == inode_id) {
          auto name = ExtractWalName(entry, *wal, pos);
          push_view(entry->inode_id_, entry->GetDirentType(), false,
                    entry->version_, name);
        }
        break;
      }
      case kWALSetattr:
      case kWALNoop:
      default:
        break;
      }

      pos += entry->SlotCount();
    }
  }

  // Sort so each name's records are adjacent with the highest version first.
  // Scope name is kept as names_build_sort for profile continuity with the
  // pre-refactor runs, even though it now covers a single O(n log n) pass
  // over POD DentViews (no string extraction).
  {
    LISTDIR_PROFILE_SCOPE(dfs::kSrvNamesBuildSort);
    std::sort(collected.begin(), collected.end(),
              [](const DentView &a, const DentView &b) {
                int c = std::strcmp(a.name_, b.name_);
                if (c != 0) {
                  return c < 0;
                }
                return a.version_ > b.version_;
              });
  }

  // Linear dedup: the first occurrence of each name is the winning op.
  // Tombstones are retained — the client needs them to arbitrate across
  // metaservers, mirroring the shape ReadDirLatest used to return.
  {
    LISTDIR_PROFILE_SCOPE(dfs::kSrvEntriesEmplace);
    entries.reserve(collected.size());
    const char *last_name = nullptr;
    for (const auto &view : collected) {
      if (last_name != nullptr && std::strcmp(view.name_, last_name) == 0) {
        continue;
      }
      last_name = view.name_;
      entries.push_back(view);
    }
  }
}

void Metadata::CollectPersistentDirents(uint64_t inode_id,
                                        std::vector<Dirent> &entries) {
  std::vector<DentView> views;
  CollectPersistentDentViews(inode_id, views);
  entries.clear();
  entries.reserve(views.size());
  for (const auto &view : views) {
    if (view.IsDeleted()) {
      continue;
    }
    entries.emplace_back(view.id_, view.pid_, view.type_, view.name_);
  }
}

void PendingListings::Release(uint64_t dir_id, uint64_t cutoff) {
  std::lock_guard<std::mutex> lock(mu_);
  map_.erase(Key{dir_id, cutoff});
}

void PendingListings::SweepExpired(std::chrono::milliseconds ttl) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = map_.begin(); it != map_.end();) {
    if (now - it->second->last_touch > ttl) {
      it = map_.erase(it);
    } else {
      ++it;
    }
  }
}

auto Metadata::SerializeDirents(const std::vector<Dirent> &entries,
                                Dirent *result_buffer, uint32_t size,
                                int offset) -> uint64_t {
  LISTDIR_PROFILE_SCOPE(dfs::kSrvSerializeDentViews);
  std::vector<char> packed;
  packed.reserve(entries.size() * sizeof(Dirent));
  for (auto dent : entries) {
    dent.SetRecLen();
    size_t old_size = packed.size();
    packed.resize(old_size + dent.reclen_);
    std::memcpy(packed.data() + old_size, &dent, dent.reclen_);
  }

  if (offset < 0 || static_cast<size_t>(offset) >= packed.size()) {
    return 0;
  }
  uint64_t read_size = std::min<uint64_t>(size, packed.size() - offset);
  std::memcpy(result_buffer, packed.data() + offset, read_size);
  return read_size;
}

auto Metadata::GetDents(uint64_t inode_id, Dirent *result_buffer, uint32_t size,
                        int offset) -> uint64_t {
  LISTDIR_PROFILE_SCOPE(dfs::kSrvGetDentViewsOuter);
  if (gCXLPersistence != nullptr) {
    std::vector<Dirent> entries;
    CollectPersistentDirents(inode_id, entries);
    return SerializeDirents(entries, result_buffer, size, offset);
  }
  return 0;
}

auto Metadata::GetDentViews(uint64_t inode_id, uint64_t read_cutoff_version,
                            DentView *result_buffer, uint32_t size,
                            int offset) -> uint64_t {
  LISTDIR_PROFILE_SCOPE(dfs::kSrvGetDentViewsOuter);
  if (gCXLPersistence == nullptr || offset < 0) {
    return 0;
  }

  // Stage the full serialized DentView stream on first contact; every
  // follow-up RPC for the same (dir_id, cutoff) hits this cache entry and
  // only pays an O(log n) binary search + one memcpy.
  auto listing = pending_listings_.GetOrStage(
      inode_id, read_cutoff_version, [&]() -> PendingListings::ListingPtr {
        auto l = std::make_shared<PendingListings::Listing>();

        std::vector<DentView> entries;
        CollectPersistentDentViews(inode_id, entries, read_cutoff_version);

        size_t total = 0;
        for (const auto &e : entries) {
          total += e.reclen_;
        }
        l->bytes.resize(total);
        l->record_offsets.reserve(entries.size() + 1);
        l->record_offsets.push_back(0);
        size_t pos = 0;
        for (const auto &e : entries) {
          std::memcpy(l->bytes.data() + pos, &e, e.reclen_);
          pos += e.reclen_;
          l->record_offsets.push_back(pos);
        }
        return l;
      });

  if (!listing) {
    return 0;
  }

  LISTDIR_PROFILE_SCOPE(dfs::kSrvSerializeDentViews);
  const size_t target = static_cast<size_t>(offset);
  if (target >= listing->bytes.size()) {
    // This is the terminating "end of stream" call. Drop the staging
    // entry now that the client has drained everything; any concurrent
    // slicers still own a live shared_ptr so their memcpy is safe.
    pending_listings_.Release(inode_id, read_cutoff_version);
    return 0;
  }

  // `target` must land on a record boundary — the client accumulates byte
  // counts returned from previous calls, which we always end on a record.
  auto beg = std::lower_bound(listing->record_offsets.begin(),
                              listing->record_offsets.end(), target);
  if (beg == listing->record_offsets.end() || *beg != target) {
    SPDLOG_ERROR("GetDentViews: non-record-aligned offset {} for dir {}",
                 target, inode_id);
    pending_listings_.Release(inode_id, read_cutoff_version);
    return 0;
  }

  // Largest record boundary <= target + size gives us the chunk end.
  const size_t limit = target + size;
  auto end_it = std::upper_bound(listing->record_offsets.begin(),
                                 listing->record_offsets.end(), limit);
  // upper_bound returns the first element strictly > limit; the preceding
  // element is the largest boundary we may stop at. Because `target` is
  // itself a valid boundary, end_it is never listing->record_offsets.begin().
  --end_it;
  const size_t end_pos = *end_it;
  if (end_pos <= target) {
    // A single record is larger than the client-provided chunk. With
    // DentView.reclen_ capped at ~92 B and chunk_size defaulting to 1 MB
    // this is never supposed to happen, but bail gracefully rather than
    // looping forever on the client.
    SPDLOG_ERROR("GetDentViews: chunk size {} too small for next record "
                 "in dir {} (offset={})",
                 size, inode_id, target);
    pending_listings_.Release(inode_id, read_cutoff_version);
    return 0;
  }
  const size_t written = end_pos - target;
  std::memcpy(result_buffer, listing->bytes.data() + target, written);
  if (end_pos == listing->bytes.size()) {
    // Served the final record in this snapshot. Release eagerly so a
    // subsequent listdir on the same (dir_id, cutoff) key rebuilds from
    // fresh state instead of reusing this exhausted staging entry —
    // important for clients (and tests) that stop their readdir loop
    // before making the terminating zero-byte call. Any concurrent
    // slicers still hold their own shared_ptr, so their memcpy is safe.
    pending_listings_.Release(inode_id, read_cutoff_version);
  }
  return written;
}

auto Metadata::LocateInode(const char *path, uint64_t *parent_buf,
                           Dirent *ent_ptr, int path_len) -> Result {
  const char *node_name;
  int offset = (path[0] == '/' ? 1 : 0);
  int name_len;
  if (path_len == -1) {
    path_len = strlen(path);
  }
  uint64_t inode_id = kRootId;
  if (parent_buf != nullptr && *parent_buf != 0) {
    inode_id = *parent_buf;
    *parent_buf = 0;
  }
  uint64_t parent_id = 0;
  bool found = false;
  Dirent d_buffer;
  Dirent *dptr;
  while (offset < path_len) {
    // Get directory / file name, and move offset to next level.
    node_name = path + offset;
    name_len = 0;
    while (offset + name_len < path_len && node_name[name_len] != '/') {
      name_len++;
    }
    offset += name_len + 1; // This 1 is for '/'

    dptr = &d_buffer; // dptr has to hold parent in ALL cases.
    bool ret;
#ifdef HASHTABLE_LATENCY_PROFILE
    dhashtable_times_.fetch_add(1);
    uint64_t st = Gethrtime();
#endif
    string s_name(node_name, name_len);
    ret = dhashtable_->find(DKeyPair(inode_id, s_name), *dptr);
#ifdef HASHTABLE_LATENCY_PROFILE
    dhashtable_time_.fetch_add(Gethrtime() - st);
#endif
    if (ret) {
      parent_id = inode_id;
      inode_id = dptr->id_;
      continue;
    }
    goto CHECK_IF_PARENT_FOUND;
  }

  // When you reach here, the path is fully parsed without missing dirs.
  if (parent_buf != nullptr) {
    *parent_buf = parent_id;
  }
  // Caution: "/" doesn't belong to any dir_table!
  if ((ent_ptr != nullptr) && (parent_id != 0)) {
    *ent_ptr = *dptr;
  }
  return Result(0, inode_id);

// Here is what we do when parent is found but node itself is not. Why a goto?
// You may ask. Well apparently I dont want two identical code sections.
CHECK_IF_PARENT_FOUND:
  if (offset >= path_len) { // We are at last level, i.e. parent found
    if (parent_buf != nullptr) {
      *parent_buf = inode_id; // Catch: parent's id at this time is in inode_id
    }
  }
  return Result(-1, 0,
                std::string(node_name, name_len)); // No such file / directory!
}

void Metadata::AddDirectoryEntry(Dirent *entry, uint64_t parent_id,
                                 bool update_parent) {
  if (update_parent) {
#ifdef HASHTABLE_LATENCY_PROFILE
    ihashtable_times_.fetch_add(1);
    uint64_t st = Gethrtime();
#endif
    if (!ihashtable_->update_fn(parent_id, [](Inode &cur) { cur.extra_++; })) {
      SPDLOG_ERROR("AddDirectoryEntry failed. parent_id = {}", parent_id);
    }
#ifdef HASHTABLE_LATENCY_PROFILE
    ihashtable_time_.fetch_add(Gethrtime() - st);
#endif
  }
#ifdef HASHTABLE_LATENCY_PROFILE
  dhashtable_times_.fetch_add(1);
  uint64_t st = Gethrtime();
#endif
  if (dhashtable_->insert(DKeyPair(parent_id, entry->name_), *entry) != 1) {
    SPDLOG_ERROR("AddDirectoryEntry failed. DHashtable Insert [{}, {}] FAILED",
                 parent_id, entry->name_);
  }
#ifdef HASHTABLE_LATENCY_PROFILE
  dhashtable_time_.fetch_add(Gethrtime() - st);
#endif
}

void Metadata::DeleteDirectoryEntry(Dirent *entry, uint64_t parent_id,
                                    bool update_parent) {
  if (update_parent) {
#ifdef HASHTABLE_LATENCY_PROFILE
    ihashtable_times_.fetch_add(1);
    uint64_t st = Gethrtime();
#endif
    if (!ihashtable_->update_fn(parent_id, [](Inode &cur) { cur.extra_--; })) {
      SPDLOG_ERROR("DeleteDirectoryEntry failed. parent_id = {}", parent_id);
    }
#ifdef HASHTABLE_LATENCY_PROFILE
    ihashtable_time_.fetch_add(Gethrtime() - st);
#endif
  }
#ifdef HASHTABLE_LATENCY_PROFILE
  dhashtable_times_.fetch_add(1);
  uint64_t st = Gethrtime();
#endif
  if (!dhashtable_->erase(DKeyPair(parent_id, entry->name_))) {
    SPDLOG_ERROR(
        "DeleteDirectoryEntry failed. DHashtable Erase [{}, {}] FAILED",
        parent_id, entry->name_);
  }
#ifdef HASHTABLE_LATENCY_PROFILE
  dhashtable_time_.fetch_add(Gethrtime() - st);
#endif
}

auto Metadata::Unlink(const char *path, bool is_dir) -> Result {
  uint64_t parent_id = 0;
  Inode inode;
  Dirent dirent;
  Result res = LocateInode(path, &parent_id, &dirent);

  if (res.mark_ == -1) {
    SPDLOG_ERROR("{}({}) failed(1). id = {}, parent = {}",
                 is_dir ? "rmdir" : "unlink", path, res.id_, parent_id);
    return Result(-1);
  }

  if (gCXLPersistence->LogUnlink(res.id_, parent_id, dirent.name_,
                                 static_cast<uint8_t>(strlen(dirent.name_))) ==
      0) {
    return Result(-EAGAIN);
  }

  bool remove = false;
  bool error = true;
  auto fn = [&remove, &error, &inode, is_dir, this](Inode &cur) {
    error = !is_dir ? (cur.mode_ & S_IFDIR) != 0
                    : (cur.mode_ & S_IFDIR) == 0 || cur.extra_ > 0;
    if (error) {
      SPDLOG_ERROR("{} failed(2). inode_id = {}, extra_ = {}, inode.mode_ = "
                   "{}, S_IFDIR = {}, inode.mode_ & S_IFDIR = {}",
                   is_dir ? "rmdir" : "unlink", cur.id_, cur.extra_, cur.mode_,
                   S_IFDIR, (cur.mode_ & S_IFDIR));
    }
    if (!error) {
      cur.nlink_--;
      remove = (cur.nlink_ == 0);
    }
    inode = cur;
  };

#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_times_.fetch_add(1);
  uint64_t st = Gethrtime();
#endif
  if (!ihashtable_->update_fn(res.id_, fn)) {
    SPDLOG_ERROR("Unlink failed. id = {}", res.id_);
  }
#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_time_.fetch_add(Gethrtime() - st);
#endif
  if (error) {
    SPDLOG_ERROR("{}({}) failed(3). error = {}", is_dir ? "rmdir" : "unlink",
                 path, error);
    return Result(-1);
  }

  DeleteDirectoryEntry(&dirent, parent_id);
  if (remove) {
    FreeInodeId(res.id_);
    DeleteInode(res.id_);
  }

  return Result(0);
};

auto Metadata::Access(const char *path, uint32_t mode, uint32_t uid,
                      uint32_t gid) -> Result {
  Result res = LocateInode(path);
  Inode inode;
  if (res.mark_ == -1 || !GetInode(res.id_, &inode)) {
    return Result(-1);
  }
  if (mode == F_OK) {
    return Result(0);
  }
  if (uid == inode.uid_) {
    return Result(static_cast<int>((mode & (inode.mode_ >> 6)) == mode) - 1);
  }
  if (gid == inode.gid_) {
    return Result(static_cast<int>((mode & (inode.mode_ >> 3)) == mode) - 1);
  }
  return Result(static_cast<int>((mode & inode.mode_) == mode) - 1);
};

auto Metadata::Chmod(const char *path, uint32_t mode, uint32_t uid,
                     uint32_t gid) -> Result {
  Result res = LocateInode(path);
  if (res.mark_ == -1 || res.id_ == 0) {
    return Result(-1);
  }

  if (gCXLPersistence->LogSetattr(res.id_, mode) == 0) {
    return Result(-EAGAIN);
  }

  bool ret;
#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_times_.fetch_add(1);
  uint64_t st = Gethrtime();
#endif
  ret =
      ihashtable_->update_fn(res.id_, [mode](Inode &cur) { cur.mode_ = mode; });
#ifdef HASHTABLE_LATENCY_PROFILE
  ihashtable_time_.fetch_add(Gethrtime() - st);
#endif
  if (ret) {
    return Result(0);
  }

  return Result(-1);
};

auto Metadata::Create(const char *path, uint32_t mode, uint32_t uid,
                      uint32_t gid, Inode *buf) -> Result {
  uint64_t id;
  uint64_t parent_id = 0;
  uint8_t type = buf != nullptr ? DT_REG : DT_DIR;

  // Now, trying to locate it should fail, but we still get its parent.
  Result res = LocateInode(path, &parent_id);

  if (parent_id == 0) { // parent dir doesnt exists.
    SPDLOG_ERROR("{}({}) failed. id = {}, parent = {}, subpath = {}",
                 type == DT_REG ? "create" : "mkdir", path, res.id_, parent_id,
                 res.new_path_.c_str());
    return Result(-1);
  }
  if (res.mark_ == 0) { // file already exists, assume O_EXCL not set.
    // TODO(): Check for O_EXCL. if set, need to return error
    SPDLOG_WARN("{}({}) error. file already exists? id = {}, parent = {}",
                type == DT_REG ? "create" : "mkdir", path, res.id_, parent_id);
    if (buf != nullptr) {
      return Result(GetInode(res.id_, buf) ? 0 : -1);
    }
    Inode inode;
    return Result(GetInode(res.id_, &inode) ? 0 : -1);
  }

  id = AllocateInodeId();
  time_t now = time(nullptr);
  if (type == DT_DIR) {
    mode |= S_IFDIR; // make sure it has a DIR flag.
  } else {
    mode |= S_IFREG; // make sure it has a REG flag.
  }

  Dirent entry(id, parent_id, type, res.new_path_.c_str());
  if (gCXLPersistence->LogCreate(
          id, parent_id, mode, uid, gid, type, res.new_path_.c_str(),
          static_cast<uint8_t>(res.new_path_.size())) == 0) {
    FreeInodeId(id);
    return Result(-EAGAIN);
  }

  if (buf != nullptr) {
    new (buf) Inode(id, 1, mode, uid, gid, now, now, now);
    PutInode(id, buf);
  } else {
    Inode node(id, 1, mode, uid, gid, now, now, now);
    PutInode(id, &node);
  }

  AddDirectoryEntry(&entry, parent_id);

  return Result(0);
};

auto Metadata::Rename(const char *oldpath, const char *newpath,
                      bool link) -> int {
  // 1. Identify last common directory.
  int last_common_dir = 0;
  for (int i = 0; newpath[i] == oldpath[i]; i++) { // '\0' is implicitly handled
    if (oldpath[i] == '/') {
      last_common_dir = i;
    }
  }
  // 2. Locate last common directory.
  std::string last_common_path(oldpath, last_common_dir);
  Result lcd = LocateInode(last_common_path.c_str());

  if (lcd.mark_ == -1) {
    SPDLOG_INFO("Rename failed: last common dir not found: {}",
                last_common_path);
    return -1;
  }
  // 3. Locate old path from last common dir.
  uint64_t old_parent = lcd.id_;
  Dirent entry;
  Result old_res = LocateInode(oldpath + last_common_dir, &old_parent, &entry);

  if (old_res.mark_ == -1) {
    SPDLOG_INFO("Rename failed: old path not found: {}({})",
                oldpath + last_common_dir, oldpath);
    return -1;
  }
  // 4. Locate new path from last common dir.
  uint64_t new_parent = lcd.id_;
  Dirent new_entry;
  Result new_res =
      LocateInode(newpath + last_common_dir, &new_parent, &new_entry);

  if (new_res.mark_ == 0) {
    // Target already exists. Apply POSIX semantics:
    //   - link(2):   must fail with EEXIST.
    //   - rename(2): cover the target iff types match; non-empty target
    //                directories are rejected by Rmdir's own precondition
    //                check inside Unlink().
    if (link) {
      SPDLOG_INFO("Link failed: new path already exists: {}({})",
                  newpath + last_common_dir, newpath);
      return -1;
    }
    bool old_is_dir = (entry.type_ == DT_DIR);
    bool new_is_dir = (new_entry.type_ == DT_DIR);
    if (old_is_dir != new_is_dir) {
      SPDLOG_INFO("Rename failed: type mismatch between old ({}) and new ({}): "
                  "{} -> {}",
                  old_is_dir ? "dir" : "file", new_is_dir ? "dir" : "file",
                  oldpath, newpath);
      return -1;
    }
    SPDLOG_WARN("Rename cover: new path already exists: {}({})",
                newpath + last_common_dir, newpath);
    Result cover_res = Unlink(newpath, new_is_dir);
    if (cover_res.mark_ == -EAGAIN) {
      return -EAGAIN;
    }
    if (cover_res.mark_ != 0) {
      SPDLOG_INFO("Rename failed: cannot cover {} (rc={})", newpath,
                  cover_res.mark_);
      return -1;
    }
    // Re-resolve. The target should now be missing; `new_res.new_path_` will
    // carry the final path component we need for LogRename/AddDirectoryEntry.
    new_res = LocateInode(newpath + last_common_dir, &new_parent);
    if (new_res.mark_ == 0) {
      SPDLOG_ERROR("Rename failed: {} still present after cover", newpath);
      return -1;
    }
  }
  if (new_parent == 0) {
    SPDLOG_INFO("Rename failed: parent doesnt exists: {}({})",
                newpath + last_common_dir, newpath);
    return -1;
  }
  bool same_parent = old_parent == new_parent;

  if (!link) { // 5. for rename, remove dir entry from old_path
    if (gCXLPersistence->LogRename(
            old_res.id_, old_parent, new_parent, entry.name_,
            static_cast<uint8_t>(strlen(entry.name_)),
            new_res.new_path_.c_str(),
            static_cast<uint8_t>(new_res.new_path_.size()), entry.type_) == 0) {
      return -EAGAIN;
    }
    DeleteDirectoryEntry(&entry, old_parent, !same_parent);

  } else { // For link(), increment inode's nlink_.
    if (gCXLPersistence->LogLink(
            old_res.id_, new_parent, new_res.new_path_.c_str(),
            static_cast<uint8_t>(new_res.new_path_.size()), entry.type_) == 0) {
      return -EAGAIN;
    }
#ifdef HASHTABLE_LATENCY_PROFILE
    ihashtable_times_.fetch_add(1);
    uint64_t st = Gethrtime();
#endif
    if (!ihashtable_->update_fn(old_res.id_,
                                [](Inode &cur) { cur.nlink_++; })) {
      return -1;
    }
#ifdef HASHTABLE_LATENCY_PROFILE
    ihashtable_time_.fetch_add(Gethrtime() - st);
#endif
  }

  // 6. put the dir entry in new_path
  entry.pid_ = new_parent;
  strcpy(entry.name_, new_res.new_path_.c_str());
  entry.reclen_ = offsetof(Dirent, name_) + strlen(entry.name_) + 1;
  AddDirectoryEntry(&entry, new_parent, !same_parent || link);

  return 0;
}

} // namespace dfs
