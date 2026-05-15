#include "server/cxl_persistence.h"

#include "common/metadata_types.h"
#include "cxl/device.h"
#include "server/rpc_server.h"
#include "spdlog/spdlog.h"
#include "threading/affinity.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace dfs {

namespace {

auto OwnerMetaForInode(uint64_t inode_id) -> int {
  return static_cast<int>(inode_id >> INODE_ID_RANGE);
}

auto InodeModeToDirentType(mode_t mode) -> uint8_t {
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

auto EntryTimestamp(const CompactLogEntry *entry) -> time_t {
  return CompactLogEntry::VersionSeconds(entry->version_);
}

void ReplayCreateInode(const CompactLogEntry *entry, SSDInodeRegion &region) {
  const auto ts = EntryTimestamp(entry);
  Inode inode(entry->inode_id_, 1, entry->mode_, entry->GetUid(),
              entry->GetGid(), ts, ts, ts);
  region.WriteInode(entry->inode_id_, &inode, sizeof(inode));
}

// All ApplyPersistedXxx return false iff the inode slot is empty (the inode
// hasn't been replayed onto SSD yet) so the caller can defer. Each does
// exactly one LoadPage via MutateInode — half the page-cache lookups of the
// old Read+Modify+Write pattern.

// Soft delete: set nlink to 0 but keep the inode so a later out-of-order
// LINK (lower version) can still increment it.
auto ApplyPersistedUnlink(SSDInodeRegion &region, uint64_t inode_id,
                          time_t ts) -> bool {
  return region.MutateInode(inode_id, [ts](Inode &i) {
    if (i.nlink_ > 0) {
      i.nlink_--;
    }
    i.ctime_ = ts;
  });
}

auto ApplyPersistedLink(SSDInodeRegion &region, uint64_t inode_id,
                        time_t ts) -> bool {
  return region.MutateInode(inode_id, [ts](Inode &i) {
    i.nlink_++;
    i.ctime_ = ts;
  });
}

auto ApplyPersistedSetattr(SSDInodeRegion &region, uint64_t inode_id,
                           uint32_t mode, time_t ts) -> bool {
  return region.MutateInode(inode_id, [mode, ts](Inode &i) {
    i.mode_ = mode;
    i.ctime_ = ts;
  });
}

auto ApplyPersistedTouchCtime(SSDInodeRegion &region, uint64_t inode_id,
                              time_t ts) -> bool {
  return region.MutateInode(inode_id, [ts](Inode &i) { i.ctime_ = ts; });
}

auto ApplyPersistedBlocksDelta(SSDInodeRegion &region, uint64_t inode_id,
                               int64_t delta) -> bool {
  return region.MutateInode(inode_id, [delta](Inode &i) {
    if (delta < 0 && i.extra_ < static_cast<size_t>(-delta)) {
      i.extra_ = 0;
    } else {
      i.extra_ = static_cast<size_t>(static_cast<int64_t>(i.extra_) + delta);
    }
  });
}

} // namespace

// ─── CXLPersistence ─────────────────────────────────────────────────

CXLPersistence::~CXLPersistence() { StopCheckpointThread(); }

int CXLPersistence::Init(CXLSSD *ssd, int server_id) {
  ssd_ = ssd;
  server_id_ = server_id;

  if (!ssd_ || !ssd_->IsInitialized()) {
    SPDLOG_ERROR("CXLPersistence: CXLSSD not initialized");
    return -1;
  }

  // WAL ring buffer on CXL-SSD DRAM
  if (wal_.Init(ssd, CXLSSD_WAL_ENTRIES, server_id) != 0) {
    SPDLOG_ERROR("CXLPersistence: WAL init failed");
    return -1;
  }
  if (remote_inode_wal_.Init(ssd, CXLSSD_REMOTE_INODE_WAL_ENTRIES) != 0) {
    SPDLOG_ERROR("CXLPersistence: remote inode WAL init failed");
    return -1;
  }

  // Inode and dirent each live in their own backing file; both address from
  // offset 0 inside their respective file.
  constexpr uint64_t GB = 1024ULL * 1024ULL * 1024ULL;
  inode_region_.Init(ssd, CXLSSD_INODE_REGION_GB * GB);
  dent_region_.Init(ssd, CXLSSD_DENT_REGION_GB * GB);

  SPDLOG_INFO("CXLPersistence: meta={} WAL={}entries inode={}GB dent={}GB",
              server_id, (size_t)CXLSSD_WAL_ENTRIES, CXLSSD_INODE_REGION_GB,
              CXLSSD_DENT_REGION_GB);
  return 0;
}

void CXLPersistence::ConfigureClusterMetaCount(int count) {
  cluster_meta_count_ = std::max(count, 1);
}

size_t CXLPersistence::TryAppendReceivedRemoteInodeChanges(
    const std::vector<RemoteInodeChange> &changes) {
  return remote_inode_wal_.TryAppendPartial(changes);
}

// ─── WAL append helpers ─────────────────────────────────────────────

uint64_t CXLPersistence::LogCreate(uint64_t inode_id, uint64_t parent_id,
                                   uint32_t mode, uint32_t uid, uint32_t gid,
                                   uint8_t type, const char *name,
                                   uint8_t name_len) {
  auto entry = CompactLogEntry::MakeCreate(inode_id, parent_id, mode, uid, gid,
                                           type, name, name_len);
  char overflow[kCompactWalContinuationDataMax *
                kCompactWalMaxContinuationSlots] = {};
  int overflow_len = entry.BuildOverflow(name, name_len, overflow);
  if (overflow_len == 0)
    return wal_.Append(entry);
  return wal_.AppendWithOverflow(entry, overflow, overflow_len);
}

uint64_t CXLPersistence::LogUnlink(uint64_t inode_id, uint64_t parent_id,
                                   const char *name, uint8_t name_len) {
  auto entry = CompactLogEntry::MakeUnlink(inode_id, parent_id, name, name_len);
  char overflow[kCompactWalContinuationDataMax *
                kCompactWalMaxContinuationSlots] = {};
  int overflow_len = entry.BuildOverflow(name, name_len, overflow);
  if (overflow_len == 0)
    return wal_.Append(entry);
  return wal_.AppendWithOverflow(entry, overflow, overflow_len);
}

uint64_t CXLPersistence::LogRename(uint64_t inode_id, uint64_t old_parent,
                                   uint64_t new_parent, const char *old_name,
                                   uint8_t old_len, const char *new_name,
                                   uint8_t new_len, uint8_t type) {
  auto entry =
      CompactLogEntry::MakeRename(inode_id, old_parent, new_parent, old_name,
                                  old_len, new_name, new_len, type);
  char overflow[kCompactWalContinuationDataMax *
                kCompactWalMaxContinuationSlots] = {};
  int ov_len =
      entry.BuildRenameOverflow(old_name, old_len, new_name, new_len, overflow);
  if (ov_len == 0)
    return wal_.Append(entry);
  return wal_.AppendWithOverflow(entry, overflow, ov_len);
}

uint64_t CXLPersistence::LogSetattr(uint64_t inode_id, uint32_t mode) {
  auto entry = CompactLogEntry::MakeSetattr(inode_id, mode);
  return wal_.Append(entry);
}

uint64_t CXLPersistence::LogLink(uint64_t inode_id, uint64_t new_parent,
                                 const char *new_name, uint8_t new_len,
                                 uint8_t type) {
  auto entry =
      CompactLogEntry::MakeLink(inode_id, new_parent, new_name, new_len, type);
  char overflow[kCompactWalContinuationDataMax *
                kCompactWalMaxContinuationSlots] = {};
  int ov_len = entry.BuildOverflow(new_name, new_len, overflow);
  if (ov_len == 0)
    return wal_.Append(entry);
  return wal_.AppendWithOverflow(entry, overflow, ov_len);
}

// ─── Name extraction from WAL entry ────────────────────────────────

int CXLPersistence::ExtractName(const CompactLogEntry *entry, uint64_t pos,
                                char *buf, int buf_size) const {
  (void)buf_size;
  int name_len = entry->name_len_;
  int inline_len = std::min(name_len, CompactLogEntry::kInlineMax);
  std::memcpy(buf, entry->name_, inline_len);

  int remaining = name_len - inline_len;
  for (uint8_t i = 0; i < entry->ContCount() && remaining > 0; ++i) {
    auto *ov = wal_.OverflowAt(pos + 1 + i);
    int copy_len = std::min(remaining, CompactLogOverflow::kDataMax);
    std::memcpy(buf + inline_len + i * CompactLogOverflow::kDataMax, ov->data_,
                copy_len);
    remaining -= copy_len;
  }
  return name_len;
}

// Helper: extract concatenated old+new names for RENAME/LINK
static void ExtractRenameNames(const CompactLogEntry *entry,
                               const CompactWAL &wal, uint64_t pos,
                               char *old_name, int &old_len, char *new_name,
                               int &new_len) {
  old_len = entry->name_len_;
  new_len = entry->aux_len_;
  int total = old_len + new_len;

  char full[128];
  int inline_total = std::min(total, CompactLogEntry::kInlineMax);
  std::memcpy(full, entry->name_, inline_total);
  int remaining = total - inline_total;
  for (uint8_t i = 0; i < entry->ContCount() && remaining > 0; ++i) {
    auto *ov = wal.OverflowAt(pos + 1 + i);
    int copy_len = std::min(remaining, CompactLogOverflow::kDataMax);
    std::memcpy(full + inline_total + i * CompactLogOverflow::kDataMax,
                ov->data_, copy_len);
    remaining -= copy_len;
  }
  std::memcpy(old_name, full, old_len);
  std::memcpy(new_name, full + old_len, new_len);
}

// ─── Checkpoint: WAL → InodeRegion + DentRegion on SSD ──────────────
bool CXLPersistence::DoCheckpoint(bool force_sync) {
  using clock = std::chrono::steady_clock;
  constexpr auto kPhaseBudgetNormal = std::chrono::microseconds(500);
  constexpr auto kPhaseBudgetPressure = std::chrono::microseconds(10000);
  constexpr auto kSyncMaxInterval = std::chrono::milliseconds(50);

  if (pending_main_pos_ < wal_.CheckpointPos()) {
    pending_main_pos_ = wal_.CheckpointPos();
  }
  if (pending_remote_pos_ < remote_inode_wal_.CheckpointPos()) {
    pending_remote_pos_ = remote_inode_wal_.CheckpointPos();
  }

  const uint64_t cp = pending_main_pos_;
  const uint64_t head = wal_.Head();
  const uint64_t remote_cp = pending_remote_pos_;
  const uint64_t remote_head = remote_inode_wal_.Head();

  const bool has_new_work =
      (cp < head) || (remote_cp < remote_head) || !deferred_remote_ops_.empty();
  bool any_pending = (pending_main_pos_ > wal_.CheckpointPos()) ||
                     (pending_remote_pos_ > remote_inode_wal_.CheckpointPos());

  const bool wal_pressure =
      (head - wal_.CheckpointPos()) >= (wal_.Capacity() / 5);
  const bool remote_pressure =
      (remote_head - remote_inode_wal_.CheckpointPos()) >=
      (remote_inode_wal_.Capacity() / 5);
  const auto phase_budget = (wal_pressure || remote_pressure)
                                ? kPhaseBudgetPressure
                                : kPhaseBudgetNormal;
  const auto now = clock::now();
  if (last_sync_ts_.time_since_epoch().count() == 0) {
    last_sync_ts_ = now;
  }
  constexpr uint64_t kSyncBatchThreshold = 32 * 1024;
  const bool timer_due = (now - last_sync_ts_ >= kSyncMaxInterval);

  if (!has_new_work && !any_pending) {
    return false;
  }
  if (!has_new_work && !force_sync && !timer_due) {
    const uint64_t prev_drained =
        (pending_main_pos_ - wal_.CheckpointPos()) +
        (pending_remote_pos_ - remote_inode_wal_.CheckpointPos());
    if (prev_drained < kSyncBatchThreshold) {
      return false;
    }
  }

  // ── Per-cycle BlocksDelta coalescer ──
  // Just a delta map — the page cache already caches the inode bytes, so
  // there's no value in mirroring them in here. At flush time we walk the
  // map and call MutateInode once per inode (single LoadPage per inode).
  std::unordered_map<uint64_t, int64_t> blocks_delta;

  auto accumulate_blocks = [&blocks_delta](uint64_t inode_id, int64_t delta) {
    blocks_delta[inode_id] += delta;
  };

  // ── main WAL replay ──
  char name_buf[128];
  std::vector<std::vector<RemoteInodeChange>> remote_batches(
      std::max(cluster_meta_count_, 1));

  auto queue_remote_change =
      [&remote_batches](int owner, RemoteInodeChangeOp op, uint64_t inode_id,
                        int64_t value, uint64_t version) {
        if (owner < 0) {
          return;
        }
        if (remote_batches.size() <= static_cast<size_t>(owner)) {
          remote_batches.resize(owner + 1);
        }
        remote_batches[owner].push_back(
            RemoteInodeChange{op, inode_id, value, version});
      };

  // Per-parent dent buffer. mdtest's "many ops under one dir per cycle"
  // pattern collapses N (lock + 2 hashmap + page memcpy) calls into one
  // SSDDentRegion::AppendEntries call after the replay loop finishes.
  // PUT and DEL events are appended in arrival order — same on-flash
  // ordering as before (DentRecordHeader.flags_ marks tombstones).
  std::unordered_map<uint64_t, std::vector<DentEntry>> dent_buffer;

  auto buffer_dent = [&dent_buffer](uint64_t parent_id, DentEntry dent) {
    dent_buffer[parent_id].push_back(std::move(dent));
  };

  const auto main_deadline = clock::now() + phase_budget;
  uint64_t pos = cp;
  while (pos < head) {
    const auto *entry = wal_.At(pos);

    // Empty / unpublished primary marks the current tail boundary.
    if (!entry->IsReady()) {
      break;
    }

    switch (entry->op_) {
    case kWALCreate: {
      ReplayCreateInode(entry, inode_region_);
      int parent_owner = OwnerMetaForInode(entry->parent_id_);
      if (parent_owner == server_id_) {
        accumulate_blocks(entry->parent_id_, 1);
      } else {
        queue_remote_change(parent_owner, kRemoteParentBlocksDelta,
                            entry->parent_id_, 1, entry->version_);
      }
      int nlen = ExtractName(entry, pos, name_buf, sizeof(name_buf));
      buffer_dent(entry->parent_id_,
                  DentEntry::Make(entry->inode_id_,
                                  entry->aux_len_, // type stored in aux_len_
                                  name_buf, (uint8_t)nlen, entry->version_));
      break;
    }

    case kWALUnlink: {
      int child_owner = OwnerMetaForInode(entry->inode_id_);
      if (child_owner == server_id_) {
        ApplyPersistedUnlink(inode_region_, entry->inode_id_,
                             EntryTimestamp(entry));
      } else {
        queue_remote_change(child_owner, kRemoteInodeUnlink, entry->inode_id_,
                            0, entry->version_);
      }
      int parent_owner = OwnerMetaForInode(entry->parent_id_);
      if (parent_owner == server_id_) {
        accumulate_blocks(entry->parent_id_, -1);
      } else {
        queue_remote_change(parent_owner, kRemoteParentBlocksDelta,
                            entry->parent_id_, -1, entry->version_);
      }
      int nlen = ExtractName(entry, pos, name_buf, sizeof(name_buf));
      buffer_dent(entry->parent_id_,
                  DentEntry::Make(entry->inode_id_, DT_UNKNOWN, name_buf,
                                  (uint8_t)nlen, entry->version_,
                                  DentEntry::kFlagTombstone));
      break;
    }

    case kWALRename: {
      char old_name[64];
      char new_name[64];
      int old_len;
      int new_len;
      uint8_t type = entry->GetDirentType();
      ExtractRenameNames(entry, wal_, pos, old_name, old_len, new_name,
                         new_len);
      int child_owner = OwnerMetaForInode(entry->inode_id_);
      // Do not modify time temporarily
      // if (child_owner == server_id_) { ... } else { ... touch_ctime ... }
      (void)child_owner;
      uint64_t new_parent = entry->GetNewParent();
      if (entry->parent_id_ != new_parent) {
        int old_parent_owner = OwnerMetaForInode(entry->parent_id_);
        if (old_parent_owner == server_id_) {
          accumulate_blocks(entry->parent_id_, -1);
        } else {
          queue_remote_change(old_parent_owner, kRemoteParentBlocksDelta,
                              entry->parent_id_, -1, entry->version_);
        }
        int new_parent_owner = OwnerMetaForInode(new_parent);
        if (new_parent_owner == server_id_) {
          accumulate_blocks(new_parent, 1);
        } else {
          queue_remote_change(new_parent_owner, kRemoteParentBlocksDelta,
                              new_parent, 1, entry->version_);
        }
      }
      buffer_dent(entry->parent_id_,
                  DentEntry::Make(entry->inode_id_, DT_UNKNOWN, old_name,
                                  (uint8_t)old_len, entry->version_,
                                  DentEntry::kFlagTombstone));
      buffer_dent(new_parent,
                  DentEntry::Make(entry->inode_id_, type, new_name,
                                  (uint8_t)new_len, entry->version_));
      break;
    }

    case kWALSetattr: {
      int child_owner = OwnerMetaForInode(entry->inode_id_);
      if (child_owner == server_id_) {
        ApplyPersistedSetattr(inode_region_, entry->inode_id_, entry->mode_,
                              EntryTimestamp(entry));
      } else {
        queue_remote_change(child_owner, kRemoteInodeSetattr, entry->inode_id_,
                            entry->mode_, entry->version_);
      }
      break;
    }

    case kWALLink: {
      uint8_t type = entry->GetDirentType();
      int child_owner = OwnerMetaForInode(entry->inode_id_);
      if (child_owner == server_id_) {
        inode_region_.MutateInode(entry->inode_id_,
                                  [ts = EntryTimestamp(entry)](Inode &i) {
                                    i.nlink_++;
                                    i.ctime_ = ts;
                                  });
      } else {
        queue_remote_change(child_owner, kRemoteInodeLink, entry->inode_id_, 0,
                            entry->version_);
      }
      char new_name_buf[64];
      int nl = ExtractName(entry, pos, new_name_buf, sizeof(new_name_buf));
      uint64_t new_parent = entry->GetNewParent();
      int new_parent_owner = OwnerMetaForInode(new_parent);
      if (new_parent_owner == server_id_) {
        accumulate_blocks(new_parent, 1);
      } else {
        queue_remote_change(new_parent_owner, kRemoteParentBlocksDelta,
                            new_parent, 1, entry->version_);
      }
      buffer_dent(new_parent,
                  DentEntry::Make(entry->inode_id_, type, new_name_buf,
                                  (uint8_t)nl, entry->version_));
      break;
    }

    default:
      break;
    }

    pos += entry->SlotCount();

    if (clock::now() >= main_deadline) {
      break;
    }
  }

  // Flush per-parent dent batches: one lock + walk-and-append per dir,
  // instead of N individual AppendEntry/DeleteEntry calls.
  for (auto &[pid, ents] : dent_buffer) {
    dent_region_.AppendEntries(pid, ents);
  }

  // ── remote WAL drain + coalesce (two-bucket) ──
  std::vector<RemoteInodeWalEntry> non_coalescable;
  std::unordered_map<uint64_t, RemoteInodeWalEntry> coalesced;
  non_coalescable.reserve(deferred_remote_ops_.size() / 4 + 16);
  coalesced.reserve(deferred_remote_ops_.size() + 64);

  auto consume = [&non_coalescable, &coalesced](const RemoteInodeWalEntry &e) {
    const auto op = static_cast<RemoteInodeChangeOp>(e.op_);
    const bool can_coalesce =
        (op == kRemoteParentBlocksDelta || op == kRemoteInodeSetattr ||
         op == kRemoteInodeTouchCtime);
    if (!can_coalesce) {
      non_coalescable.push_back(e);
      return;
    }
    const uint64_t key = (e.inode_id_ << 4) | (e.op_ & 0xf);
    auto [it, inserted] = coalesced.emplace(key, e);
    if (inserted) {
      return;
    }
    auto &cur = it->second;
    if (op == kRemoteParentBlocksDelta) {
      cur.value_ += e.value_;
      if (e.version_ > cur.version_) {
        cur.version_ = e.version_;
      }
    } else if (e.version_ > cur.version_) {
      cur.value_ = e.value_;
      cur.version_ = e.version_;
    }
  };

  // Feed deferred (cross-cycle retries) first, then newly-drained entries.
  for (const auto &e : deferred_remote_ops_) {
    consume(e);
  }
  deferred_remote_ops_.clear();

  uint64_t remote_pos = remote_cp;
  while (remote_pos < remote_head) {
    const auto *entry = remote_inode_wal_.At(remote_pos);
    if (!entry->IsReady()) {
      break;
    }
    consume(*entry);
    remote_pos++;
  }
  const uint64_t remote_drain_end = remote_pos;

  // Materialize: non-coalescable first, then coalesced values.
  std::vector<RemoteInodeWalEntry> remote_entries;
  remote_entries.reserve(non_coalescable.size() + coalesced.size());
  remote_entries = std::move(non_coalescable);
  for (auto &kv : coalesced) {
    remote_entries.push_back(std::move(kv.second));
  }
  coalesced.clear();

  // Bucket entries by 4K page offset so all ops touching the same cached
  // page are applied back-to-back (one LoadPage per page instead of per op).
  // Within a bucket, sort by (inode_id, version) so per-inode order is
  // preserved — cross-inode order across buckets is irrelevant since each
  // inode's history is contained in exactly one bucket.
  std::unordered_map<uint64_t, std::vector<uint32_t>> page_buckets;
  page_buckets.reserve(remote_entries.size() / 8 + 16);
  for (uint32_t i = 0; i < remote_entries.size(); ++i) {
    const uint64_t off =
        inode_region_.PageOffsetFor(remote_entries[i].inode_id_);
    page_buckets[off].push_back(i);
  }
  for (auto &kv : page_buckets) {
    auto &idxs = kv.second;
    std::sort(idxs.begin(), idxs.end(), [&](uint32_t a, uint32_t b) {
      const auto &ea = remote_entries[a];
      const auto &eb = remote_entries[b];
      if (ea.inode_id_ != eb.inode_id_) {
        return ea.inode_id_ < eb.inode_id_;
      }
      return ea.version_ < eb.version_;
    });
  }

  auto apply_one = [this](const RemoteInodeWalEntry &entry) -> bool {
    switch (static_cast<RemoteInodeChangeOp>(entry.op_)) {
    case kRemoteInodeUnlink:
      return ApplyPersistedUnlink(
          inode_region_, entry.inode_id_,
          CompactLogEntry::VersionSeconds(entry.version_));
    case kRemoteInodeLink:
      return ApplyPersistedLink(
          inode_region_, entry.inode_id_,
          CompactLogEntry::VersionSeconds(entry.version_));
    case kRemoteInodeSetattr:
      return ApplyPersistedSetattr(
          inode_region_, entry.inode_id_, static_cast<uint32_t>(entry.value_),
          CompactLogEntry::VersionSeconds(entry.version_));
    case kRemoteInodeTouchCtime:
      return ApplyPersistedTouchCtime(
          inode_region_, entry.inode_id_,
          CompactLogEntry::VersionSeconds(entry.version_));
    case kRemoteParentBlocksDelta:
      return ApplyPersistedBlocksDelta(inode_region_, entry.inode_id_,
                                       entry.value_);
    default:
      return true;
    }
  };

  // Apply page-by-page, deadline-aware. When the budget runs out mid-loop,
  // any unprocessed entries (current bucket's tail + remaining buckets) get
  // pushed back into deferred_remote_ops_ for the next cycle.
  const auto remote_deadline = clock::now() + phase_budget;
  bool deadline_hit = false;
  auto bucket_it = page_buckets.begin();
  for (; bucket_it != page_buckets.end(); ++bucket_it) {
    for (uint32_t idx : bucket_it->second) {
      const auto &entry = remote_entries[idx];
      if (!apply_one(entry)) {
        deferred_remote_ops_.push_back(entry);
      }
    }
    if (clock::now() >= remote_deadline) {
      ++bucket_it;
      deadline_hit = true;
      break;
    }
  }
  if (deadline_hit) {
    for (; bucket_it != page_buckets.end(); ++bucket_it) {
      for (uint32_t idx : bucket_it->second) {
        deferred_remote_ops_.push_back(remote_entries[idx]);
      }
    }
  }

  constexpr size_t kDeferredHardLimit = CXLSSD_REMOTE_INODE_WAL_ENTRIES;
  if (deferred_remote_ops_.size() > kDeferredHardLimit) {
    const size_t drop = deferred_remote_ops_.size() - kDeferredHardLimit;
    SPDLOG_WARN("CXLPersistence: deferred_remote_ops_ at {}, dropping {} "
                "oldest entries (likely permanently-unmet dependencies)",
                deferred_remote_ops_.size(), drop);
    auto cmp = [](const RemoteInodeWalEntry &a, const RemoteInodeWalEntry &b) {
      return a.version_ < b.version_;
    };
    std::nth_element(deferred_remote_ops_.begin(),
                     deferred_remote_ops_.begin() + drop,
                     deferred_remote_ops_.end(), cmp);
    // Tally op kinds in the soon-to-be-dropped prefix to see what kind of
    // dependency keeps stalling (Create-not-yet-replayed vs anything else).
    size_t cnt[6] = {};
    for (size_t i = 0; i < drop; ++i) {
      const uint8_t op = deferred_remote_ops_[i].op_;
      if (op < 6) {
        cnt[op]++;
      }
    }
    SPDLOG_WARN("  drop op breakdown: noop={} unlink={} link={} setattr={} "
                "touchctime={} blocksdelta={}",
                cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
    deferred_remote_ops_.erase(deferred_remote_ops_.begin(),
                               deferred_remote_ops_.begin() + drop);
  }

  // ── Flush coalesced BlocksDelta to inode_region_ (one MutateInode per inode)
  // ──
  for (const auto &[id, delta] : blocks_delta) {
    if (delta == 0) {
      continue;
    }
    ApplyPersistedBlocksDelta(inode_region_, id, delta);
  }

  // ── Promote per-cycle results into across-cycle pending state ──
  if (pending_remote_batches_.size() < remote_batches.size()) {
    pending_remote_batches_.resize(remote_batches.size());
  }
  for (size_t t = 0; t < remote_batches.size(); ++t) {
    if (remote_batches[t].empty()) {
      continue;
    }
    auto &dst = pending_remote_batches_[t];
    dst.insert(dst.end(), std::make_move_iterator(remote_batches[t].begin()),
               std::make_move_iterator(remote_batches[t].end()));
  }
  pending_main_pos_ = pos;
  pending_remote_pos_ = remote_drain_end;

  const uint64_t drained_unflushed =
      (pending_main_pos_ - wal_.CheckpointPos()) +
      (pending_remote_pos_ - remote_inode_wal_.CheckpointPos());
  const bool batch_full = drained_unflushed >= kSyncBatchThreshold;
  const bool sync_due =
      (drained_unflushed > 0) && (force_sync || batch_full || timer_due);

  if (!sync_due) {
    return true;
  }

#ifdef CHECKPOINT_STATS_PROFILE
  // Snapshot batched-entry count BEFORE checkpoint advance so the telemetry
  // reflects exactly what this sync flushed.
  const uint64_t batched_entries = drained_unflushed;
  PageFlushStats inode_flush;
  PageFlushStats dent_flush;
  auto t0 = clock::now();
  inode_flush = inode_region_.FlushDirty();
  ssd_->InodeSync();
  uint64_t inode_us =
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0)
          .count();
  const auto dent_t0 = clock::now();
  dent_flush = dent_region_.FlushDirty();
  ssd_->DentSync();
  uint64_t dent_us = std::chrono::duration_cast<std::chrono::microseconds>(
                         clock::now() - dent_t0)
                         .count();
  uint64_t wall_us = inode_us + dent_us;
#else
  inode_region_.FlushDirty();
  ssd_->InodeSync();
  dent_region_.FlushDirty();
  ssd_->DentSync();
#endif

  std::lock_guard<std::mutex> lk(pending_forward_mu_);
  if (pending_forward_.size() < pending_remote_batches_.size()) {
    pending_forward_.resize(pending_remote_batches_.size());
  }
  for (size_t target = 0; target < pending_remote_batches_.size(); ++target) {
    if (static_cast<int>(target) == server_id_ ||
        pending_remote_batches_[target].empty()) {
      continue;
    }
    auto &dst = pending_forward_[target];
    dst.insert(dst.end(),
               std::make_move_iterator(pending_remote_batches_[target].begin()),
               std::make_move_iterator(pending_remote_batches_[target].end()));
  }

  const uint64_t persisted_main_cp = wal_.CheckpointPos();
  if (pending_main_pos_ > persisted_main_cp) {
    wal_.ClearRange(persisted_main_cp, pending_main_pos_);
    wal_.AdvanceCheckpoint(pending_main_pos_);
  }
  const uint64_t persisted_remote_cp = remote_inode_wal_.CheckpointPos();
  if (pending_remote_pos_ > persisted_remote_cp) {
    remote_inode_wal_.ClearRange(persisted_remote_cp, pending_remote_pos_);
    remote_inode_wal_.AdvanceCheckpoint(pending_remote_pos_);
  }

  for (auto &v : pending_remote_batches_) {
    v.clear();
  }

#ifdef CHECKPOINT_STATS_PROFILE
  // ── Rolling sync telemetry ──
  constexpr uint64_t kSyncStatsWindow = 32;
  if (sync_window_start_.time_since_epoch().count() == 0) {
    sync_window_start_ = now;
  }
  const uint64_t interval_us =
      std::chrono::duration_cast<std::chrono::microseconds>(now - last_sync_ts_)
          .count();
  sync_count_++;
  sync_acc_inode_us_ += inode_us;
  sync_acc_dent_us_ += dent_us;
  sync_acc_wall_us_ += wall_us;
  sync_acc_entries_ += batched_entries;
  sync_acc_interval_us_ += interval_us;
  sync_acc_inode_pages_ += inode_flush.pages;
  sync_acc_inode_bytes_ += inode_flush.bytes;
  sync_acc_dent_pages_ += dent_flush.pages;
  sync_acc_dent_bytes_ += dent_flush.bytes;
  if (wall_us > sync_max_wall_us_) {
    sync_max_wall_us_ = wall_us;
  }
  if (sync_count_ % kSyncStatsWindow == 0) {
    const auto window_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - sync_window_start_)
            .count();
    const double avg_inode_ms = sync_acc_inode_us_ / 1000.0 / kSyncStatsWindow;
    const double avg_dent_ms = sync_acc_dent_us_ / 1000.0 / kSyncStatsWindow;
    const double avg_wall_ms = sync_acc_wall_us_ / 1000.0 / kSyncStatsWindow;
    const double max_wall_ms = sync_max_wall_us_ / 1000.0;
    const double avg_interval_ms =
        sync_acc_interval_us_ / 1000.0 / kSyncStatsWindow;
    const double avg_batch = static_cast<double>(sync_acc_entries_) /
                             static_cast<double>(kSyncStatsWindow);
    const double avg_inode_pages =
        sync_acc_inode_pages_ / static_cast<double>(kSyncStatsWindow);
    const double avg_dent_pages =
        sync_acc_dent_pages_ / static_cast<double>(kSyncStatsWindow);
    const uint64_t total_bytes = sync_acc_inode_bytes_ + sync_acc_dent_bytes_;
    const double mb_per_s = total_bytes / (window_us > 0 ? window_us : 1) * 1.0;
    const double sync_per_s =
        kSyncStatsWindow * 1e6 / (window_us > 0 ? window_us : 1);
    // Write amplification = real disk bytes / "logical" bytes (entries × 128B).
    // <1 means coalescing helped (multiple entries hit same page); >1 means
    // sparse writes touched more page area than the entries' raw size.
    const double logical_bytes =
        static_cast<double>(sync_acc_entries_) * sizeof(Inode);
    const double write_amp =
        logical_bytes > 0 ? total_bytes / logical_bytes : 0.0;
    SPDLOG_INFO("sync stats[{}]: wall={:.2f}ms (max={:.2f}) inode={:.2f}ms "
                "dent={:.2f}ms | interval={:.2f}ms batch={:.0f} entries | "
                "pages: inode={:.0f} dent={:.0f} | {:.1f} sync/s {:.1f} MB/s "
                "wamp={:.2f}x",
                kSyncStatsWindow, avg_wall_ms, max_wall_ms, avg_inode_ms,
                avg_dent_ms, avg_interval_ms, avg_batch, avg_inode_pages,
                avg_dent_pages, sync_per_s, mb_per_s, write_amp);
    SPDLOG_INFO("Wal: checkpoint = {}, head = {}, left = {}", cp, head,
                wal_.Capacity() - head + cp);
    SPDLOG_INFO("RemoteWal: checkpoint = {}, head = {}, left = {}, defer = {}",
                remote_cp, remote_head,
                remote_inode_wal_.Capacity() - remote_head + remote_cp,
                deferred_remote_ops_.size());

    sync_acc_inode_us_ = 0;
    sync_acc_dent_us_ = 0;
    sync_acc_wall_us_ = 0;
    sync_acc_entries_ = 0;
    sync_acc_interval_us_ = 0;
    sync_max_wall_us_ = 0;
    sync_acc_inode_pages_ = 0;
    sync_acc_inode_bytes_ = 0;
    sync_acc_dent_pages_ = 0;
    sync_acc_dent_bytes_ = 0;
    sync_window_start_ = now;
  }
#endif

  last_sync_ts_ = now;
  return true;
}

void CXLPersistence::StartCheckpointThread(int idle_sleep_ms, int core_id) {
  if (checkpoint_running_.load())
    return;
  checkpoint_running_.store(true);
  checkpoint_thread_ = std::thread(&CXLPersistence::CheckpointLoop, this,
                                   idle_sleep_ms, core_id);
  SPDLOG_INFO("CXLPersistence: checkpoint thread started (idle_sleep={}ms)",
              idle_sleep_ms);
}

void CXLPersistence::StopCheckpointThread() {
  if (!checkpoint_running_.load())
    return;
  checkpoint_running_.store(false);
  if (checkpoint_thread_.joinable())
    checkpoint_thread_.join();
  while (DoCheckpoint(/*force_sync=*/true)) {
  }
  SPDLOG_INFO("CXLPersistence: checkpoint thread stopped");
}

void CXLPersistence::CheckpointLoop(int idle_sleep_ms, int core_id) {
  if (core_id >= 0) {
    dfs::BindToCore(static_cast<size_t>(core_id));
  }

  // When DoCheckpoint() can't make progress for this many consecutive cycles
  // AND there's still an unready slot at cp, we treat that slot as an orphan
  // publish (writer crashed mid-append) and skip past it.
  constexpr int kStallRounds = 5;
  const auto idle_sleep = std::chrono::milliseconds(idle_sleep_ms);

  uint64_t last_cp = wal_.CheckpointPos();
  int stall = 0;

  while (checkpoint_running_.load()) {
    const bool did_work = DoCheckpoint();

    if (!did_work) {
      std::this_thread::sleep_for(idle_sleep);
      stall = 0;
      last_cp = wal_.CheckpointPos();
      continue;
    }

    const uint64_t cp = wal_.CheckpointPos();
    const uint64_t head = wal_.Head();

    if (cp == last_cp && cp < head) {
      if (++stall >= kStallRounds) {
        const auto *e = wal_.At(cp);
        if (!e->IsReady()) {
          SPDLOG_WARN("CXLPersistence: checkpoint stalled at pos={} "
                      "(head={}, lag={} slots); skipping orphan slot "
                      "after {} idle cycles",
                      cp, head, head - cp, stall);
          wal_.AdvanceCheckpoint(cp + 1);
          wal_.ClearRange(cp, cp + 1);
        }
        stall = 0;
      }
    } else {
      stall = 0;
    }
    last_cp = cp;
  }
}

// ─── Global init/destroy ────────────────────────────────────────────

CXLPersistence *gCXLPersistence = nullptr;

void InitCXLPersistence(int meta_num, int core_id, int idle_sleep_ms) {
  if (gCXLPersistence != nullptr)
    return;

  gCXLPersistence = new CXLPersistence();
  if (gCXLPersistence->Init(gDevice->ssd, meta_num) != 0) {
    SPDLOG_ERROR("CXLPersistence init failed");
    exit(1);
  }
  gCXLPersistence->StartCheckpointThread(idle_sleep_ms, core_id);
  SPDLOG_INFO("CXLPersistence initialized for meta {}", meta_num);
}

void DestroyCXLPersistence() {
  if (gCXLPersistence != nullptr) {
    delete gCXLPersistence;
    gCXLPersistence = nullptr;
  }
}

} // namespace dfs
