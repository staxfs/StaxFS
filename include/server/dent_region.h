#pragma once

// SSD DentRegion — Directory entry persistence on SSD flash.
//
// Structure:
//   DirPage (4KB SSD write unit) = small fixed header + contiguous
//   variable-length dent records
//   Each directory has a linked list of DirPages on SSD.
//   Entry slots are append-only within a page: both PUT and DEL events are
//   appended after the current tail so page writes stay contiguous and
//   deterministic.
//
// Checkpoint path:
//   WAL CREATE  → DentRegion::AppendEntry (append PUT event)
//   WAL UNLINK  → DentRegion::DeleteEntry (append DEL event)
//   WAL RENAME  → DeleteEntry(old) + AppendEntry(new)
//
// Readdir path:
//   DentRegion::ReadDir (read DirPages from SSD) + WAL tail merge

#include "common/metadata_types.h"
#include "cxl/cxl_ssd.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace dfs {

// Stats returned by SSD*Region::FlushDirty(): how many 4K pages were
// pwritten and how many bytes that translates to. Lets the checkpoint
// telemetry report real disk write volume instead of approximating from
// WAL entry count.
struct PageFlushStats {
  size_t pages = 0;
  size_t bytes = 0;
};

// ─── DentEntry: logical compact dirent ───────────────────────────────
//
// On-flash layout is:
//   DentRecordHeader + name bytes + '\0'
//
// flags: bit0 = tombstone (deleted)
// version: the WAL version at which this event was generated
//   (used for local / cross-MDS ordering)

struct DentEntry {
  uint64_t inode_id_ = 0;
  uint64_t version_ = 0;
  uint8_t type_ = DT_UNKNOWN;
  uint8_t flags_ = 0;
  std::string name_;

  static constexpr uint8_t kFlagTombstone = 1;
  static constexpr int kNameMax = 64;

  bool IsDeleted() const { return flags_ & kFlagTombstone; }

  void MarkDeleted(uint64_t version) {
    flags_ |= kFlagTombstone;
    version_ = version;
  }

  static DentEntry Make(uint64_t inode_id, uint8_t type, const char *name,
                        uint8_t name_len, uint64_t version, uint8_t flags = 0) {
    DentEntry e{};
    e.inode_id_ = inode_id;
    e.type_ = type;
    e.version_ = version;
    e.flags_ = flags;
    e.name_.assign(name, std::min<size_t>(name_len, kNameMax - 1));
    return e;
  }
};

struct __attribute__((packed)) DentRecordHeader {
  uint64_t inode_id_;
  uint64_t version_;
  uint16_t reclen_;
  uint8_t type_;  // DT_REG / DT_DIR
  uint8_t flags_; // bit0: tombstone, event is DEL when set

  static constexpr uint8_t kFlagTombstone = DentEntry::kFlagTombstone;

  bool IsDeleted() const { return flags_ & kFlagTombstone; }

  void MarkDeleted(uint64_t version) {
    flags_ |= kFlagTombstone;
    version_ = version;
  }
};

static_assert(sizeof(DentRecordHeader) == 20,
              "DentRecordHeader must stay compact");

// ─── DirPageHeader ──────────────────────────────────────────────────

struct __attribute__((packed)) DirPageHeader {
  uint64_t dir_id_;        // 8B: directory inode id
  uint64_t next_page_off_; // 8B: offset of next page in chain (0 = end)
  uint16_t used_bytes_;    // 2B: bytes occupied in data_
  uint16_t entry_count_;   // 2B: append-only record count (PUT + DEL)
  // 2B: append-only count of PUT records written to this page. NOT a
  // "current live set size" — it is never decremented when a later DEL
  // supersedes a PUT, and it does not know about DELs that supersede
  // PUTs on *other* pages. The authoritative live view requires a
  // ReadDirLatest() pass that merges by version_.
  uint16_t put_count_;
};

// ─── DirPage (4KB) ─────────────────────────────────────────────────

struct __attribute__((packed)) DirPage {
  static constexpr size_t kPageSize = 4096;
  static constexpr size_t kHeaderSize = sizeof(DirPageHeader);
  static constexpr size_t kDataBytes = kPageSize - kHeaderSize;

  DirPageHeader header_;
  char data_[kDataBytes];

  bool HasSpace(size_t entry_size) const {
    return header_.used_bytes_ + entry_size <= kDataBytes;
  }
};

static_assert(sizeof(DirPage) == 4096, "DirPage must be 4KB");

// ─── SSD DentRegion ─────────────────────────────────────────────────
//
// Manages DirPages on SSD flash. Each directory gets a linked list of pages.
// Pages are allocated from a contiguous SSD flash region via bump allocator.
// The "compact" property here refers to the on-flash layout: entries are
// appended densely within each 4KB page and pages are chained per directory.
// It does not mean the persistence design depends only on C++ struct packing.
//
// Thread safety: checkpoint thread is the sole writer; readers (readdir)
// only read committed pages, so no locking needed.

class SSDDentRegion {
public:
  SSDDentRegion() = default;

  int Init(CXLSSD *ssd, uint64_t capacity_bytes) {
    ssd_ = ssd;
    capacity_ = capacity_bytes;
    // Reserve 0 as the null page offset for page-chain termination.
    alloc_pos_ = DirPage::kPageSize;
    num_page_slots_ = capacity_bytes / DirPage::kPageSize;

    pool_ = std::make_unique<PageCacheEntry[]>(kPageCacheMaxPages);
    for (uint32_t i = 0; i < kPageCacheMaxPages; ++i) {
      pool_[i].next_ = (i + 1 < kPageCacheMaxPages) ? (i + 1) : kNullIdx;
    }
    free_head_ = 0;

    page_slots_.reset(new uint32_t[num_page_slots_]);
    std::fill_n(page_slots_.get(), num_page_slots_, kNullIdx);

    SPDLOG_INFO("SSDDentRegion: initialized (capacity={}MB)",
                capacity_ / (1024 * 1024));
    return 0;
  }

  // ── Checkpoint operations (called by checkpoint thread) ──

  // Append a directory event from WAL checkpoint. Allocates new page if needed.
  void AppendEntry(uint64_t dir_id, const DentEntry &entry) {
    std::unique_lock lock(mu_);
    AppendEntryLocked(dir_id, entry);
  }

  // Append a tombstone event by parent_id + name.
  bool DeleteEntry(uint64_t dir_id, uint64_t inode_id, const char *name,
                   uint8_t name_len, uint64_t version) {
    std::unique_lock lock(mu_);
    AppendEntryLocked(dir_id,
                      DentEntry::Make(inode_id, DT_UNKNOWN, name, name_len,
                                      version, DentEntry::kFlagTombstone));
    return true;
  }

  // Batch append: takes the lock once, walks the head-page chain in place
  // (no Read→modify→Write copies), allocating new pages only when the
  // current one is full. For mdtest's "many ops per parent in one cycle"
  // pattern this collapses N (lock + 2 hashmap + 2 memcpy) calls into one.
  void AppendEntries(uint64_t dir_id, const std::vector<DentEntry> &entries) {
    if (entries.empty())
      return;
    std::unique_lock lock(mu_);

    uint64_t page_off = GetOrCreateHeadPage(dir_id);
    uint32_t pool_idx = LoadPageSlot(page_off);
    DirPage *page = &pool_[pool_idx].page_;

    for (const auto &entry : entries) {
      const uint16_t record_size = static_cast<uint16_t>(
          sizeof(DentRecordHeader) + entry.name_.size() + 1);

      if (!page->HasSpace(record_size)) {
        // Current head full — alloc a new head page and chain the old one.
        const uint64_t new_off = AllocPage();
        const uint32_t new_idx = LoadPageSlotForFresh(new_off);
        DirPage *new_page = &pool_[new_idx].page_;
        std::memset(new_page, 0, sizeof(DirPage));
        new_page->header_.dir_id_ = dir_id;
        new_page->header_.next_page_off_ = page_off;
        dir_index_[dir_id] = new_off;
        page_off = new_off;
        pool_idx = new_idx;
        page = new_page;
      }

      auto *record = reinterpret_cast<DentRecordHeader *>(
          page->data_ + page->header_.used_bytes_);
      record->inode_id_ = entry.inode_id_;
      record->version_ = entry.version_;
      record->reclen_ = record_size;
      record->type_ = entry.type_;
      record->flags_ = entry.flags_;
      char *name = reinterpret_cast<char *>(record + 1);
      std::memcpy(name, entry.name_.data(), entry.name_.size());
      name[entry.name_.size()] = '\0';
      page->header_.used_bytes_ += record_size;
      page->header_.entry_count_++;
      if (!entry.IsDeleted()) {
        page->header_.put_count_++;
      }
      pool_[pool_idx].dirty_ = true;
    }
  }

  // ── Readdir (called by metadata server threads) ──

  // Read the latest local event per name for a directory, including
  // tombstones. Caller can further merge with WAL tail or other metas.
  void ReadDirLatest(uint64_t dir_id,
                     std::unordered_map<std::string, DentEntry> &latest) const {
    ReadDirLatest(dir_id, std::numeric_limits<uint64_t>::max(), latest);
  }

  void ReadDirLatest(uint64_t dir_id, uint64_t read_cutoff_version,
                     std::unordered_map<std::string, DentEntry> &latest) const {
    std::shared_lock lock(mu_);
    auto it = dir_index_.find(dir_id);
    if (it == dir_index_.end())
      return;

    uint64_t page_off = it->second;
    while (page_off != 0) {
      DirPage page;
      ReadPage(page_off, page);
      size_t pos = 0;
      uint16_t seen = 0;
      while (pos + sizeof(DentRecordHeader) <= page.header_.used_bytes_ &&
             seen < page.header_.entry_count_) {
        const auto *record =
            reinterpret_cast<const DentRecordHeader *>(page.data_ + pos);
        if (record->reclen_ < sizeof(DentRecordHeader) ||
            pos + record->reclen_ > page.header_.used_bytes_) {
          const auto used_bytes = page.header_.used_bytes_;
          const auto reclen = record->reclen_;
          SPDLOG_ERROR("SSDDentRegion: corrupt dir page dir_id={} page_off={} "
                       "used={} pos={} reclen={}",
                       dir_id, page_off, used_bytes, pos, reclen);
          break;
        }
        DentEntry entry{};
        entry.inode_id_ = record->inode_id_;
        entry.version_ = record->version_;
        entry.type_ = record->type_;
        entry.flags_ = record->flags_;
        entry.name_.assign(reinterpret_cast<const char *>(record + 1),
                           record->reclen_ - sizeof(*record) - 1);
        if (entry.version_ > read_cutoff_version) {
          pos += record->reclen_;
          ++seen;
          continue;
        }
        auto iter = latest.find(entry.name_);
        if (iter == latest.end() || iter->second.version_ < entry.version_) {
          latest[entry.name_] = std::move(entry);
        }
        pos += record->reclen_;
        ++seen;
      }
      page_off = page.header_.next_page_off_;
    }
  }

  // Read raw events for a directory into a flat DentView vector.
  // Callers are expected to sort by (name, version desc) and dedup linearly.
  void ReadDirLatestFlat(uint64_t dir_id, uint64_t read_cutoff_version,
                         std::vector<DentView> &out) const {
    std::shared_lock lock(mu_);
    auto it = dir_index_.find(dir_id);
    if (it == dir_index_.end()) {
      return;
    }

    uint64_t page_off = it->second;
    while (page_off != 0) {
      DirPage page;
      ReadPage(page_off, page);
      size_t pos = 0;
      uint16_t seen = 0;
      while (pos + sizeof(DentRecordHeader) <= page.header_.used_bytes_ &&
             seen < page.header_.entry_count_) {
        const auto *record =
            reinterpret_cast<const DentRecordHeader *>(page.data_ + pos);
        if (record->reclen_ < sizeof(DentRecordHeader) ||
            pos + record->reclen_ > page.header_.used_bytes_) {
          const auto used_bytes = page.header_.used_bytes_;
          const auto reclen = record->reclen_;
          SPDLOG_ERROR("SSDDentRegion: corrupt dir page dir_id={} page_off={} "
                       "used={} pos={} reclen={}",
                       dir_id, page_off, used_bytes, pos, reclen);
          break;
        }
        if (record->version_ <= read_cutoff_version) {
          out.emplace_back();
          auto &view = out.back();
          view.id_ = record->inode_id_;
          view.pid_ = dir_id;
          view.version_ = record->version_;
          view.type_ = record->type_;
          view.flags_ = (record->flags_ & DentRecordHeader::kFlagTombstone)
                            ? DentView::kFlagTombstone
                            : 0;
          size_t name_len = record->reclen_ - sizeof(*record) - 1;
          if (name_len >= sizeof(view.name_)) {
            name_len = sizeof(view.name_) - 1;
          }
          std::memcpy(view.name_, record + 1, name_len);
          view.name_[name_len] = '\0';
          view.SetRecLen();
        }
        pos += record->reclen_;
        ++seen;
      }
      page_off = page.header_.next_page_off_;
    }
  }

  // Flush all dirty cached pages to flash. Caller must follow with
  // CXLSSD::DentSync() to make the writes durable. Returned stats include
  // both pages flushed here AND any dirty victims pwritten inline by LRU
  // eviction since the previous FlushDirty.
  PageFlushStats FlushDirty() {
    std::unique_lock lock(mu_);
    PageFlushStats s;
    s.pages = evict_pages_;
    s.bytes = evict_bytes_;
    evict_pages_ = 0;
    evict_bytes_ = 0;
    for (uint32_t idx = lru_head_; idx != kNullIdx; idx = pool_[idx].next_) {
      auto &e = pool_[idx];
      if (e.dirty_) {
        ssd_->DentWrite(e.offset_, &e.page_, DirPage::kPageSize);
        e.dirty_ = false;
        s.pages++;
        s.bytes += DirPage::kPageSize;
      }
    }
    return s;
  }

  uint64_t AllocPos() const { return alloc_pos_; }

private:
  void AppendEntryLocked(uint64_t dir_id, const DentEntry &entry) {
    const uint16_t record_size = static_cast<uint16_t>(
        sizeof(DentRecordHeader) + entry.name_.size() + 1);
    uint64_t page_off = GetOrCreateHeadPage(dir_id);
    uint32_t pool_idx = LoadPageSlot(page_off);
    DirPage *page = &pool_[pool_idx].page_;

    if (!page->HasSpace(record_size)) {
      const uint64_t new_off = AllocPage();
      const uint32_t new_idx = LoadPageSlotForFresh(new_off);
      DirPage *new_page = &pool_[new_idx].page_;
      std::memset(new_page, 0, sizeof(DirPage));
      new_page->header_.dir_id_ = dir_id;
      new_page->header_.next_page_off_ = page_off;
      dir_index_[dir_id] = new_off;
      page_off = new_off;
      pool_idx = new_idx;
      page = new_page;
    }

    auto *record = reinterpret_cast<DentRecordHeader *>(
        page->data_ + page->header_.used_bytes_);
    record->inode_id_ = entry.inode_id_;
    record->version_ = entry.version_;
    record->reclen_ = record_size;
    record->type_ = entry.type_;
    record->flags_ = entry.flags_;
    char *name = reinterpret_cast<char *>(record + 1);
    std::memcpy(name, entry.name_.data(), entry.name_.size());
    name[entry.name_.size()] = '\0';
    page->header_.used_bytes_ += record_size;
    page->header_.entry_count_++;
    if (!entry.IsDeleted()) {
      page->header_.put_count_++;
    }
    pool_[pool_idx].dirty_ = true;
  }

  uint64_t AllocPage() {
    uint64_t off = alloc_pos_;
    alloc_pos_ += DirPage::kPageSize;
    if (alloc_pos_ > capacity_) {
      SPDLOG_ERROR("SSDDentRegion: out of space (alloc_pos={}, cap={})",
                   alloc_pos_, capacity_);
    }
    return off;
  }

  uint64_t GetOrCreateHeadPage(uint64_t dir_id) {
    auto it = dir_index_.find(dir_id);
    if (it != dir_index_.end())
      return it->second;
    // First page for this directory.
    uint64_t off = AllocPage();
    uint32_t idx = LoadPageSlotForFresh(off);
    auto &e = pool_[idx];
    std::memset(&e.page_, 0, sizeof(DirPage));
    e.page_.header_.dir_id_ = dir_id;
    e.dirty_ = true;
    dir_index_[dir_id] = off;
    return off;
  }

  // ── Pool-backed page cache (write-back) ──
  // page_slots_[page_off / kPageSize] → pool index (kNullIdx = miss).
  // Mutations happen in-place on pool_[idx].page_; pwrite is deferred
  // until FlushDirty() so consecutive ops on one DirPage coalesce into
  // a single 4K disk write.

  void LruUnlink(uint32_t idx) {
    auto &e = pool_[idx];
    if (e.prev_ != kNullIdx)
      pool_[e.prev_].next_ = e.next_;
    else
      lru_head_ = e.next_;
    if (e.next_ != kNullIdx)
      pool_[e.next_].prev_ = e.prev_;
    else
      lru_tail_ = e.prev_;
    e.prev_ = kNullIdx;
    e.next_ = kNullIdx;
  }

  void LruPushFront(uint32_t idx) {
    auto &e = pool_[idx];
    e.prev_ = kNullIdx;
    e.next_ = lru_head_;
    if (lru_head_ != kNullIdx)
      pool_[lru_head_].prev_ = idx;
    else
      lru_tail_ = idx;
    lru_head_ = idx;
  }

  // Evict tail or take a free slot. Returns a pool index whose payload is
  // uninitialized — caller fills in offset_/page_/dirty_ and links into LRU.
  uint32_t TakePoolSlot() {
    if (free_head_ != kNullIdx) {
      uint32_t idx = free_head_;
      free_head_ = pool_[idx].next_;
      pool_[idx].next_ = kNullIdx;
      return idx;
    }
    // Evict LRU tail.
    uint32_t idx = lru_tail_;
    auto &v = pool_[idx];
    if (v.dirty_) {
      ssd_->DentWrite(v.offset_, &v.page_, DirPage::kPageSize);
      evict_pages_++;
      evict_bytes_ += DirPage::kPageSize;
    }
    page_slots_[v.offset_ / DirPage::kPageSize] = kNullIdx;
    LruUnlink(idx);
    return idx;
  }

  // Bring `offset` into cache, reading from disk on miss.
  uint32_t LoadPageSlot(uint64_t offset) {
    const uint64_t page_idx = offset / DirPage::kPageSize;
    uint32_t pool_idx = page_slots_[page_idx];
    if (pool_idx != kNullIdx) {
      if (pool_idx != lru_head_) {
        LruUnlink(pool_idx);
        LruPushFront(pool_idx);
      }
      return pool_idx;
    }
    pool_idx = TakePoolSlot();
    auto &e = pool_[pool_idx];
    e.offset_ = offset;
    e.dirty_ = false;
    ssd_->DentRead(offset, &e.page_, DirPage::kPageSize);
    page_slots_[page_idx] = pool_idx;
    LruPushFront(pool_idx);
    return pool_idx;
  }

  // Same as LoadPageSlot but skips the disk read (caller will overwrite the
  // entire page contents — used for newly bump-allocated pages).
  uint32_t LoadPageSlotForFresh(uint64_t offset) {
    const uint64_t page_idx = offset / DirPage::kPageSize;
    uint32_t pool_idx = page_slots_[page_idx];
    if (pool_idx != kNullIdx) {
      if (pool_idx != lru_head_) {
        LruUnlink(pool_idx);
        LruPushFront(pool_idx);
      }
      return pool_idx;
    }
    pool_idx = TakePoolSlot();
    auto &e = pool_[pool_idx];
    e.offset_ = offset;
    e.dirty_ = false;
    page_slots_[page_idx] = pool_idx;
    LruPushFront(pool_idx);
    return pool_idx;
  }

  // Read-only helper for readdir paths. No LRU mutation (readers don't own
  // the LRU); on miss, returns a copy from disk without populating cache.
  void ReadPage(uint64_t offset, DirPage &page) const {
    const uint64_t page_idx = offset / DirPage::kPageSize;
    uint32_t pool_idx = page_slots_[page_idx];
    if (pool_idx != kNullIdx) {
      page = pool_[pool_idx].page_;
      return;
    }
    std::memset(&page, 0, sizeof(page));
    ssd_->DentRead(offset, &page, DirPage::kPageSize);
  }

  CXLSSD *ssd_ = nullptr;
  uint64_t capacity_ = 0;
  uint64_t alloc_pos_ = 0; // bump allocator position (file-local)
  uint64_t num_page_slots_ = 0;

  // In-memory index: dir_id → head page offset (rebuilt on recovery)
  std::unordered_map<uint64_t, uint64_t> dir_index_;

  // 16K pages × 4 KB = 64 MB cap per meta.
  static constexpr size_t kPageCacheMaxPages = 16 * 1024;
  static constexpr uint32_t kNullIdx = UINT32_MAX;

  struct PageCacheEntry {
    uint64_t offset_ = 0;
    uint32_t prev_ = kNullIdx;
    uint32_t next_ = kNullIdx;
    bool dirty_ = false;
    DirPage page_{};
  };

  mutable std::unique_ptr<PageCacheEntry[]> pool_;
  mutable std::unique_ptr<uint32_t[]> page_slots_;
  mutable uint32_t lru_head_ = kNullIdx;
  mutable uint32_t lru_tail_ = kNullIdx;
  mutable uint32_t free_head_ = kNullIdx;

  mutable std::shared_mutex mu_;

  // Pages/bytes pwritten by inline LRU eviction since the last FlushDirty.
  // Folded into the next FlushDirty's return value, then reset.
  size_t evict_pages_ = 0;
  size_t evict_bytes_ = 0;
};

} // namespace dfs
