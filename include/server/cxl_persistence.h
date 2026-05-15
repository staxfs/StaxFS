#pragma once

// CXL Persistence Layer — WAL-based metadata persistence on CXL-SSD.

#include "common/metadata_types.h"
#include "cxl/cxl_ssd.h"
#include "server/compact_wal.h"
#include "server/dent_region.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dfs {

// Layout: inode_id → slot at file-local offset (inode_id % max_inodes) * 128B.
//
// Page cache implementation:
//   - page_slots_: dense array indexed by (page_off / kPageSize) → pool index
//     (or kNullIdx). One uint32 per possible page in the region (~16MB for
//     a 16GB region). Zero hashing, pointer-free direct lookup.
//   - pool_: pre-allocated fixed-size array of PageCacheEntry (each embeds
//     a full 4KB page). Contiguous memory, no per-entry heap allocation.
//   - Intrusive doubly-linked LRU via pool indices in prev/next — splice
//     is 4 index writes into already-hot pool memory, vs the old std::list
//     which chased pointers into heap nodes.
class SSDInodeRegion {
public:
  SSDInodeRegion() = default;

  int Init(CXLSSD *ssd, uint64_t capacity_bytes) {
    ssd_ = ssd;
    capacity_ = capacity_bytes;
    max_inodes_ = capacity_bytes / kSlotSize;
    num_page_slots_ = capacity_bytes / kPageSize;

    pool_ = std::make_unique<PageCacheEntry[]>(kPageCacheMaxPages);
    // Thread the free list through pool_[i].next.
    for (uint32_t i = 0; i < kPageCacheMaxPages; ++i) {
      pool_[i].next = (i + 1 < kPageCacheMaxPages) ? (i + 1) : kNullIdx;
    }
    free_head_ = 0;

    page_slots_.reset(new uint32_t[num_page_slots_]);
    std::fill_n(page_slots_.get(), num_page_slots_, kNullIdx);

    lru_head_ = kNullIdx;
    lru_tail_ = kNullIdx;
    num_cached_ = 0;
    return 0;
  }

  void WriteInode(uint64_t inode_id, const void *data, size_t len) {
    auto *entry = LoadPage(inode_id);
    std::memcpy(entry->page.slots + InPageOffset(inode_id), data, len);
    entry->dirty = true;
  }

  void ReadInode(uint64_t inode_id, void *buf, size_t len) {
    auto *entry = LoadPage(inode_id);
    std::memcpy(buf, entry->page.slots + InPageOffset(inode_id), len);
  }

  // Apply an in-place mutation to the cached inode. Returns false if the
  // slot is empty (id_ == 0) so the caller can defer. Saves the second
  // LoadPage that the Read+Modify+Write pattern would otherwise pay.
  template <class Fn>
  bool MutateInode(uint64_t inode_id, Fn &&fn) {
    auto *entry = LoadPage(inode_id);
    auto *inode =
        reinterpret_cast<Inode *>(entry->page.slots + InPageOffset(inode_id));
    if (inode->id_ == 0) {
      return false;
    }
    fn(*inode);
    entry->dirty = true;
    return true;
  }

  // Map an inode_id to the 4K page offset that holds it. Exposed so the
  // checkpoint apply loop can bucket entries by page before applying.
  uint64_t PageOffsetFor(uint64_t inode_id) const {
    return PageOffset(inode_id);
  }

  // Flush every dirty cached page to flash. Caller must follow with
  // CXLSSD::InodeSync() to make the writes durable. Returned stats include
  // both pages flushed here AND any dirty victims pwritten inline by LRU
  // eviction since the previous FlushDirty (so the telemetry sees real
  // disk traffic, not just sync-time traffic).
  PageFlushStats FlushDirty() {
    PageFlushStats s;
    s.pages = evict_pages_;
    s.bytes = evict_bytes_;
    evict_pages_ = 0;
    evict_bytes_ = 0;
    for (uint32_t idx = lru_head_; idx != kNullIdx; idx = pool_[idx].next) {
      auto &e = pool_[idx];
      if (e.dirty) {
        ssd_->InodeWrite(e.offset, &e.page, kPageSize);
        e.dirty = false;
        s.pages++;
        s.bytes += kPageSize;
      }
    }
    return s;
  }

  uint64_t Capacity() const { return capacity_; }

private:
  static constexpr size_t kSlotSize = 128;
  static constexpr size_t kPageSize = 4096;
  static constexpr size_t kSlotsPerPage = kPageSize / kSlotSize; // 32
  // 64K pages × 4 KB = 256 MB cap (matches dent_region budget).
  static constexpr size_t kPageCacheMaxPages = 64 * 1024;
  static constexpr uint32_t kNullIdx = UINT32_MAX;

  struct alignas(64) InodePage {
    char slots[kPageSize];
  };

  struct PageCacheEntry {
    uint64_t offset = 0;
    uint32_t prev = kNullIdx; // LRU prev pool index (or free-list unused)
    uint32_t next = kNullIdx; // LRU next pool index / free-list next
    bool dirty = false;
    InodePage page{};
  };

  uint64_t PageOffset(uint64_t inode_id) const {
    const uint64_t slot = inode_id % max_inodes_;
    return (slot / kSlotsPerPage) * kPageSize;
  }

  size_t InPageOffset(uint64_t inode_id) const {
    return ((inode_id % max_inodes_) % kSlotsPerPage) * kSlotSize;
  }

  void LruUnlink(uint32_t idx) {
    auto &e = pool_[idx];
    if (e.prev != kNullIdx)
      pool_[e.prev].next = e.next;
    else
      lru_head_ = e.next;
    if (e.next != kNullIdx)
      pool_[e.next].prev = e.prev;
    else
      lru_tail_ = e.prev;
    e.prev = kNullIdx;
    e.next = kNullIdx;
  }

  void LruPushFront(uint32_t idx) {
    auto &e = pool_[idx];
    e.prev = kNullIdx;
    e.next = lru_head_;
    if (lru_head_ != kNullIdx)
      pool_[lru_head_].prev = idx;
    else
      lru_tail_ = idx;
    lru_head_ = idx;
  }

  PageCacheEntry *LoadPage(uint64_t inode_id) {
    const uint64_t off = PageOffset(inode_id);
    const uint64_t page_idx = off / kPageSize;
    uint32_t pool_idx = page_slots_[page_idx];

    if (pool_idx != kNullIdx) {
      // Hit — move to LRU front (unless already there).
      if (pool_idx != lru_head_) {
        LruUnlink(pool_idx);
        LruPushFront(pool_idx);
      }
      return &pool_[pool_idx];
    }

    // Miss — evict LRU tail or take from free list.
    if (num_cached_ >= kPageCacheMaxPages) {
      pool_idx = lru_tail_;
      auto &v = pool_[pool_idx];
      if (v.dirty) {
        ssd_->InodeWrite(v.offset, &v.page, kPageSize);
        evict_pages_++;
        evict_bytes_ += kPageSize;
      }
      page_slots_[v.offset / kPageSize] = kNullIdx;
      LruUnlink(pool_idx);
    } else {
      pool_idx = free_head_;
      free_head_ = pool_[pool_idx].next;
      pool_[pool_idx].next = kNullIdx;
      ++num_cached_;
    }

    auto &e = pool_[pool_idx];
    e.offset = off;
    e.dirty = false;
    ssd_->InodeRead(off, &e.page, kPageSize);
    page_slots_[page_idx] = pool_idx;
    LruPushFront(pool_idx);
    return &e;
  }

  CXLSSD *ssd_ = nullptr;
  uint64_t capacity_ = 0;
  uint64_t max_inodes_ = 0;
  uint64_t num_page_slots_ = 0;

  std::unique_ptr<PageCacheEntry[]> pool_;
  std::unique_ptr<uint32_t[]> page_slots_;
  uint32_t lru_head_ = kNullIdx;
  uint32_t lru_tail_ = kNullIdx;
  uint32_t free_head_ = kNullIdx;
  size_t num_cached_ = 0;

  // Pages/bytes pwritten by inline LRU eviction since the last FlushDirty.
  // Folded into the next FlushDirty's return value, then reset.
  size_t evict_pages_ = 0;
  size_t evict_bytes_ = 0;
};

struct alignas(64) RemoteInodeWalEntry {
  uint32_t seq_ = 0;
  uint8_t op_ = kRemoteInodeNoop;
  uint8_t reserved0_ = 0;
  uint16_t reserved1_ = 0;
  uint64_t version_ = 0;
  uint64_t inode_id_ = 0;
  int64_t value_ = 0;
  char padding_[32] = {};

  bool IsReady() const { return seq_ != 0; }
};

static_assert(sizeof(RemoteInodeWalEntry) == 64,
              "RemoteInodeWalEntry must be 64B");

class RemoteInodeWAL {
public:
  int Init(CXLSSD *ssd, size_t capacity_entries) {
    ssd_ = ssd;
    capacity_ = capacity_entries;
    capacity_mask_ = capacity_entries - 1;
    uint64_t alloc_size = capacity_ * sizeof(RemoteInodeWalEntry);
    ring_dram_offset_ = ssd->DramAlloc(alloc_size, 64);
    if (ring_dram_offset_ == UINT64_MAX) {
      SPDLOG_ERROR("RemoteInodeWAL: failed to allocate {}MB from CXL-SSD DRAM",
                   alloc_size / (1024 * 1024));
      return -1;
    }
    ring_ = reinterpret_cast<RemoteInodeWalEntry *>(
        static_cast<char *>(ssd->DramBase()) + ring_dram_offset_);

    SPDLOG_INFO("RemoteInodeWAL: capacity={} entries ({}MB)", capacity_,
                alloc_size / (1024 * 1024));

    return 0;
  }

  size_t TryAppendPartial(const std::vector<RemoteInodeChange> &changes) {
    if (changes.empty())
      return 0;
    const size_t want = changes.size();
    uint64_t cur_head = head_.load(std::memory_order_relaxed);
    size_t take;
    uint64_t new_head;
    for (;;) {
      uint64_t cp = checkpoint_pos_.load(std::memory_order_acquire);
      uint64_t in_use = cur_head - cp;
      if (in_use >= capacity_ - 1) {
        return 0;
      }
      uint64_t free_slots = capacity_ - 1 - in_use;
      take = std::min<size_t>(want, free_slots);
      new_head = cur_head + take;
      if (head_.compare_exchange_weak(cur_head, new_head,
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        break;
      }
    }
    for (size_t i = 0; i < take; ++i) {
      WriteEntry(cur_head + i, changes[i]);
    }
    return take;
  }

  const RemoteInodeWalEntry *At(uint64_t pos) const {
    return &ring_[pos & capacity_mask_];
  }

  void ClearRange(uint64_t begin, uint64_t end) {
    alignas(64) char zero[64] = {};
    for (uint64_t pos = begin; pos < end; ++pos) {
      DramWrite(pos, zero, 64);
    }
  }

  void AdvanceCheckpoint(uint64_t new_pos) {
    checkpoint_pos_.store(new_pos, std::memory_order_release);
  }

  uint64_t CheckpointPos() const {
    return checkpoint_pos_.load(std::memory_order_acquire);
  }

  uint64_t Head() const { return head_.load(std::memory_order_acquire); }

  size_t Capacity() const { return capacity_; }

private:
  void DramWrite(uint64_t slot_pos, const void *data, uint64_t size) const {
    uint64_t offset = ring_dram_offset_ +
                      (slot_pos & capacity_mask_) * sizeof(RemoteInodeWalEntry);
    ssd_->DramWrite(offset, size, const_cast<void *>(data));
  }

  void WriteEntry(uint64_t pos, const RemoteInodeChange &change) {
    RemoteInodeWalEntry entry;
    entry.seq_ = local_seq_.fetch_add(1, std::memory_order_relaxed);
    if (entry.seq_ == 0) {
      entry.seq_ = local_seq_.fetch_add(1, std::memory_order_relaxed);
    }
    entry.op_ = static_cast<uint8_t>(change.op_);
    entry.version_ = change.version_;
    entry.inode_id_ = change.inode_id_;
    entry.value_ = change.value_;
    DramWrite(pos, &entry, sizeof(entry));
  }

  CXLSSD *ssd_ = nullptr;
  RemoteInodeWalEntry *ring_ = nullptr;
  uint64_t ring_dram_offset_ = 0;
  size_t capacity_ = 0;
  size_t capacity_mask_ = 0;
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> checkpoint_pos_{0};
  std::atomic<uint32_t> local_seq_{1};
};

// WAL-based metadata persistence on CXL-SSD.
class CXLPersistence {
public:
  CXLPersistence() = default;
  ~CXLPersistence();

  int Init(CXLSSD *ssd, int server_id);

  // ── WAL access ──
  CompactWAL *WAL() { return &wal_; }

  RemoteInodeWAL *RemoteWAL() { return &remote_inode_wal_; }

  // ── High-level WAL append helpers ──

  uint64_t LogCreate(uint64_t inode_id, uint64_t parent_id, uint32_t mode,
                     uint32_t uid, uint32_t gid, uint8_t type, const char *name,
                     uint8_t name_len);

  uint64_t LogUnlink(uint64_t inode_id, uint64_t parent_id, const char *name,
                     uint8_t name_len);

  uint64_t LogRename(uint64_t inode_id, uint64_t old_parent,
                     uint64_t new_parent, const char *old_name, uint8_t old_len,
                     const char *new_name, uint8_t new_len, uint8_t type);

  uint64_t LogSetattr(uint64_t inode_id, uint32_t mode);

  uint64_t LogLink(uint64_t inode_id, uint64_t new_parent, const char *new_name,
                   uint8_t new_len, uint8_t type);

  // ── Checkpoint ──
  // The checkpoint thread runs DoCheckpoint() back-to-back when there is
  // work; `idle_sleep_ms` is only the back-off when a cycle finds nothing
  // to do. There is no fixed cadence under load.
  void StartCheckpointThread(int idle_sleep_ms, int core_id = -1);
  void StopCheckpointThread();

  // ── SSD regions ──
  SSDInodeRegion *InodeRegion() { return &inode_region_; }

  SSDDentRegion *DentRegion() { return &dent_region_; }

  void ConfigureClusterMetaCount(int count);

  // Non-blocking partial-insert used by the inbound handler. Returns count
  // actually inserted.
  size_t TryAppendReceivedRemoteInodeChanges(
      const std::vector<RemoteInodeChange> &changes);

  // ── Outbound forward queue (drained by guardian thread) ──
  std::mutex &PendingForwardMu() { return pending_forward_mu_; }

  std::vector<std::vector<RemoteInodeChange>> &PendingForward() {
    return pending_forward_;
  }

  std::vector<size_t> &ForwardBatchCap() { return forward_batch_cap_; }

  int ServerId() const { return server_id_; }

private:
  void CheckpointLoop(int idle_sleep_ms, int core_id);
  // Returns true iff this cycle did real work (replayed a main-WAL entry,
  // applied or deferred a remote-WAL entry, or flushed pending unsynced
  // state). The caller uses this to decide whether to back off when idle.
  // `force_sync = true` ignores the time/pressure threshold and always
  // flushes pending state in this cycle (used at shutdown to drain).
  bool DoCheckpoint(bool force_sync = false);

  // Extract full name from WAL entry (inline + overflow).
  int ExtractName(const CompactLogEntry *entry, uint64_t pos, char *buf,
                  int buf_size) const;

  CXLSSD *ssd_ = nullptr;
  int server_id_ = 0;
  int cluster_meta_count_ = 1;
  CompactWAL wal_;
  RemoteInodeWAL remote_inode_wal_;
  SSDInodeRegion inode_region_;
  SSDDentRegion dent_region_;

  std::thread checkpoint_thread_;
  std::atomic<bool> checkpoint_running_{false};

  // Remote ops deferred because the target inode was not yet on SSD.
  // Retried (version-sorted) on the next checkpoint cycle.
  std::vector<RemoteInodeWalEntry> deferred_remote_ops_;

  std::mutex pending_forward_mu_;
  std::vector<std::vector<RemoteInodeChange>> pending_forward_;
  std::vector<size_t> forward_batch_cap_;

  // ── Group-commit state ──
  // Replay/apply runs every cycle; FlushDirty + InodeSync + DentSync +
  // AdvanceCheckpoint only run when sync_due (drained ≥ batch threshold,
  // timer expired, or force_sync). Until then, advance positions and
  // outbound batches accumulate here so the next cycle picks up where this
  // one left off without re-reading the same ring slots.
  std::chrono::steady_clock::time_point last_sync_ts_{};
  uint64_t pending_main_pos_ = 0;
  uint64_t pending_remote_pos_ = 0;
  std::vector<std::vector<RemoteInodeChange>> pending_remote_batches_;

#ifdef CHECKPOINT_STATS_PROFILE
  // ── Per-sync telemetry (rolling window, logged every kSyncStatsWindow) ──
  uint64_t sync_count_ = 0;
  uint64_t sync_acc_inode_us_ = 0;
  uint64_t sync_acc_dent_us_ = 0;
  uint64_t sync_acc_wall_us_ = 0;
  uint64_t sync_acc_entries_ = 0;
  uint64_t sync_acc_interval_us_ = 0;
  uint64_t sync_max_wall_us_ = 0;
  uint64_t sync_acc_inode_pages_ = 0;
  uint64_t sync_acc_inode_bytes_ = 0;
  uint64_t sync_acc_dent_pages_ = 0;
  uint64_t sync_acc_dent_bytes_ = 0;
  std::chrono::steady_clock::time_point sync_window_start_{};
#endif
};

extern CXLPersistence *gCXLPersistence;

void InitCXLPersistence(int meta_num, int core_id = -1,
                        int idle_sleep_ms = CXLSSD_CHECKPOINT_INTERVAL_MS);
void DestroyCXLPersistence();

} // namespace dfs
