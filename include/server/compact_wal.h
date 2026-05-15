#pragma once

// Compact WAL (Write-Ahead Log) on CXL-SSD DRAM.
//
// CompactLogEntry: 64B cache-line-aligned WAL entry encoding 5 metadata ops.
// CompactWAL: Per-MDS ring buffer with atomic head, clwb persistence.
//
// Write path:  reserve slots → write continuations → publish primary
// Latency:     ~300ns per 64B CXL-SSD DRAM write
// Overflow:    Long names spill into one or more 64B continuation entries.
// Commit:      primary seq_ encodes ready bit + continuation count + local seq.
// Checkpoint:  Background thread scans [checkpoint_pos_, head_) → SSD flash.
//
// Recovery needs per entry:
//   CREATE:  inode_id, parent_id, mode, uid, gid, type, name, version
//   UNLINK:  inode_id, parent_id, name, version
//   RENAME:  inode_id, old_parent, new_parent, old_name, new_name, version
//   SETATTR: inode_id, mode, version
//   LINK:    inode_id, new_parent, new_name, version

#include "cxl/cxl_ssd.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <immintrin.h>
#include <spdlog/spdlog.h>

namespace dfs {

// ─── WAL Operation Types ────────────────────────────────────────────

enum WALOp : uint8_t {
  kWALNoop = 0,    // padding / overflow continuation
  kWALCreate = 1,  // create file or directory
  kWALUnlink = 2,  // unlink / rmdir
  kWALRename = 3,  // rename
  kWALSetattr = 4, // chmod / chown
  kWALLink = 5,    // hard link
};

constexpr int kCompactWalContinuationDataMax = 56;
constexpr uint8_t kCompactWalMaxContinuationSlots = 3;

// ─── CompactLogEntry (64B) ──────────────────────────────────────────
//
// Layout:
//   [0:4)   seq_        — ready bit + continuation count + local seq
//   [4]     op_         — WALOp
//   [5]     flags_      — reserved for future use
//   [6]     name_len_   — name length (RENAME: old_name_len)
//   [7]     aux_len_    — RENAME: new_name_len; CREATE: type
//   [8:16)  version_    — packed HLC version
//   [16:24) inode_id_   — target inode
//   [24:32) parent_id_  — parent dir (RENAME: old_parent, LINK: new_parent)
//   [32:40) aux_        — per-op: see table below
//   [40:44) mode_       — na'j
//   [44:64) name_[20]   — inline name payload
//
// aux_ encoding per op:
//   CREATE:  uid(4B high) | gid(4B low)
//   UNLINK:  0
//   RENAME:  new_parent_id (8B)
//   SETATTR: 0
//   LINK:    0
//
// Continuation data is stored in the immediately following slots.
//
// Recovery example (CREATE):
//   Inode(inode_id_, nlink=1, mode_, GetUid(), GetGid(),
//         VersionSeconds(version_), ..., size=0, blocks=0)

struct alignas(64) CompactLogEntry {
  uint32_t seq_;       // ready + continuation count + local seq
  uint8_t op_;         // WALOp
  uint8_t flags_;      // reserved
  uint8_t name_len_;   // primary name length
  uint8_t aux_len_;    // RENAME: new_name_len; CREATE: type
  uint64_t version_;   // packed HLC version
  uint64_t inode_id_;  // target inode id
  uint64_t parent_id_; // parent dir id (RENAME: old_parent, LINK: new_parent)
  uint64_t aux_;       // per-op auxiliary (see table above)
  uint32_t mode_;      // CREATE/SETATTR: mode
  char name_[20];      // inline name

  // ── Constants ──
  static constexpr int kInlineMax = 20;
  static constexpr uint32_t kSeqReadyMask = 1u << 31;
  static constexpr uint32_t kSeqContShift = 29;
  static constexpr uint32_t kSeqContMask = 0x3u << kSeqContShift;
  static constexpr uint32_t kSeqLocalMask = (1u << kSeqContShift) - 1;

  static constexpr uint8_t kVersionMetaBits = 8;
  static constexpr uint8_t kVersionLogicalBits = 14;
  static constexpr uint8_t kVersionPhysicalShift =
      kVersionMetaBits + kVersionLogicalBits;

  bool IsReady() const { return (seq_ & kSeqReadyMask) != 0; }

  uint8_t ContCount() const {
    return static_cast<uint8_t>((seq_ & kSeqContMask) >> kSeqContShift);
  }

  uint32_t LocalSeq() const { return seq_ & kSeqLocalMask; }

  int SlotCount() const { return 1 + ContCount(); }

  static uint32_t EncodeSeq(uint32_t local_seq, uint8_t cont_count) {
    assert(local_seq > 0);
    assert((local_seq & ~kSeqLocalMask) == 0);
    assert(cont_count <= kCompactWalMaxContinuationSlots);
    return kSeqReadyMask | (uint32_t(cont_count) << kSeqContShift) | local_seq;
  }

  static uint64_t PackVersion(uint64_t physical_ms, uint16_t logical,
                              uint8_t meta_id) {
    return (physical_ms << kVersionPhysicalShift) |
           (uint64_t(logical) << kVersionMetaBits) | uint64_t(meta_id);
  }

  static uint64_t VersionPhysicalMs(uint64_t version) {
    return version >> kVersionPhysicalShift;
  }

  static time_t VersionSeconds(uint64_t version) {
    return static_cast<time_t>(VersionPhysicalMs(version) / 1000);
  }

  // ── CREATE aux packing: uid(4B high) | gid(4B low) ──
  static uint64_t PackCreateAux(uint32_t uid, uint32_t gid) {
    return ((uint64_t)uid << 32) | (uint64_t)gid;
  }

  uint32_t GetUid() const { return (uint32_t)(aux_ >> 32); }

  uint32_t GetGid() const { return (uint32_t)(aux_); }

  uint64_t GetNewParent() const { return op_ == kWALLink ? parent_id_ : aux_; }

  uint8_t GetDirentType() const {
    if (op_ == kWALCreate) {
      return aux_len_;
    }
    if (op_ == kWALRename || op_ == kWALLink) {
      return static_cast<uint8_t>(mode_ & 0xff);
    }
    return DT_UNKNOWN;
  }

  // ── Builders ──
  // NOTE: seq_ and version_ are set by CompactWAL::Append(), not here.

  static CompactLogEntry MakeCreate(uint64_t inode_id, uint64_t parent_id,
                                    uint32_t mode, uint32_t uid, uint32_t gid,
                                    uint8_t type, const char *name,
                                    uint8_t name_len) {
    CompactLogEntry e{};
    e.op_ = kWALCreate;
    e.inode_id_ = inode_id;
    e.parent_id_ = parent_id;
    e.mode_ = mode;
    e.aux_ = PackCreateAux(uid, gid);
    e.aux_len_ = type; // CREATE stores dirent type in aux_len_
    e.name_len_ = name_len;
    int inline_len = std::min((int)name_len, kInlineMax);
    std::memcpy(e.name_, name, inline_len);
    return e;
  }

  static CompactLogEntry MakeUnlink(uint64_t inode_id, uint64_t parent_id,
                                    const char *name, uint8_t name_len) {
    CompactLogEntry e{};
    e.op_ = kWALUnlink;
    e.inode_id_ = inode_id;
    e.parent_id_ = parent_id;
    e.name_len_ = name_len;
    int inline_len = std::min((int)name_len, kInlineMax);
    std::memcpy(e.name_, name, inline_len);
    return e;
  }

  static CompactLogEntry MakeSetattr(uint64_t inode_id, uint32_t mode) {
    CompactLogEntry e{};
    e.op_ = kWALSetattr;
    e.inode_id_ = inode_id;
    e.mode_ = mode;
    return e;
  }

  // RENAME/LINK: old_name + new_name concatenated in name_[] + overflow
  static CompactLogEntry MakeRename(uint64_t inode_id, uint64_t old_parent,
                                    uint64_t new_parent, const char *old_name,
                                    uint8_t old_len, const char *new_name,
                                    uint8_t new_len, uint8_t type) {
    CompactLogEntry e{};
    e.op_ = kWALRename;
    e.inode_id_ = inode_id;
    e.parent_id_ = old_parent;
    e.aux_ = new_parent;
    e.mode_ = type;
    e.name_len_ = old_len;
    e.aux_len_ = new_len;
    int total = old_len + new_len;
    // Copy old_name first, then new_name
    int old_inline = std::min((int)old_len, kInlineMax);
    std::memcpy(e.name_, old_name, old_inline);
    int remaining = kInlineMax - old_inline;
    if (remaining > 0 && new_len > 0) {
      int new_inline = std::min((int)new_len, remaining);
      std::memcpy(e.name_ + old_inline, new_name, new_inline);
    }
    return e;
  }

  static CompactLogEntry MakeLink(uint64_t inode_id, uint64_t new_parent,
                                  const char *new_name, uint8_t new_len,
                                  uint8_t type) {
    CompactLogEntry e{};
    e.op_ = kWALLink;
    e.inode_id_ = inode_id;
    e.parent_id_ = new_parent;
    e.mode_ = type;
    e.name_len_ = new_len;
    int inline_len = std::min((int)new_len, kInlineMax);
    std::memcpy(e.name_, new_name, inline_len);
    return e;
  }

  // Build overflow data buffer for names exceeding kInlineMax.
  // Caller provides sufficiently large continuation buffer.
  int BuildOverflow(const char *name, uint8_t name_len, char *buf) const {
    if (name_len <= kInlineMax)
      return 0;
    int overflow_len = name_len - kInlineMax;
    std::memcpy(buf, name + kInlineMax, overflow_len);
    return overflow_len;
  }

  // For RENAME: build overflow from concatenated old+new names
  int BuildRenameOverflow(const char *old_name, uint8_t old_len,
                          const char *new_name, uint8_t new_len,
                          char *buf) const {
    int total = old_len + new_len;
    if (total <= kInlineMax)
      return 0;
    // Reconstruct the full concatenated buffer
    char concat[128];
    std::memcpy(concat, old_name, old_len);
    std::memcpy(concat + old_len, new_name, new_len);
    int overflow_len = total - kInlineMax;
    std::memcpy(buf, concat + kInlineMax, overflow_len);
    return overflow_len;
  }
};

static_assert(sizeof(CompactLogEntry) == 64, "CompactLogEntry must be 64B");

// ─── Overflow Continuation (64B) ────────────────────────────────────

struct alignas(64) CompactLogOverflow {
  uint32_t magic_; // kOverflowMagic: identifies this as overflow (not a normal
                   // entry)
  uint32_t parent_seq_; // low 29 bits store primary local_seq
  char data_[kCompactWalContinuationDataMax]; // continuation name data

  static constexpr uint32_t kOverflowMagic = 0x4F564652; // "OVFR"
  static constexpr int kDataMax = kCompactWalContinuationDataMax;

  bool IsValid(uint32_t expected_local_seq) const {
    return magic_ == kOverflowMagic && parent_seq_ == expected_local_seq;
  }
};

static_assert(sizeof(CompactLogOverflow) == 64,
              "CompactLogOverflow must be 64B");

// ─── CompactWAL: Ring Buffer on CXL-SSD DRAM ───────────────────────

class CompactWAL {
public:
  CompactWAL() = default;

  // Allocate ring buffer from CXL-SSD DRAM.
  // capacity_entries must be a power of 2.
  int Init(CXLSSD *ssd, size_t capacity_entries, int mds_id) {
    assert((capacity_entries & (capacity_entries - 1)) == 0);
    ssd_ = ssd;
    capacity_ = capacity_entries;
    capacity_mask_ = capacity_entries - 1;
    mds_id_ = mds_id;

    uint64_t alloc_size = capacity_ * sizeof(CompactLogEntry);
    ring_dram_offset_ = ssd->DramAlloc(alloc_size, 64);
    if (ring_dram_offset_ == UINT64_MAX) {
      SPDLOG_ERROR("CompactWAL: failed to allocate {}MB from CXL-SSD DRAM",
                   alloc_size / (1024 * 1024));
      return -1;
    }
    // Keep raw pointer for reads (At / OverflowAt)
    ring_ = reinterpret_cast<CompactLogEntry *>(
        static_cast<char *>(ssd->DramBase()) + ring_dram_offset_);

    SPDLOG_INFO("CompactWAL: mds={} capacity={}entries ({}MB)", mds_id,
                capacity_, alloc_size / (1024 * 1024));
    return 0;
  }

  uint64_t TryReserveSlots(uint64_t slots_needed) {
    uint64_t cur_head = head_.load(std::memory_order_relaxed);
    for (;;) {
      uint64_t cp = checkpoint_pos_.load(std::memory_order_acquire);
      if (cur_head + slots_needed - cp >= capacity_) {
        return UINT64_MAX;
      }
      if (head_.compare_exchange_weak(cur_head, cur_head + slots_needed,
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        return cur_head;
      }
    }
  }

  // Cost: 1× DramWrite(64B) ≈ 600ns.
  uint64_t Append(CompactLogEntry &entry) {
    uint64_t pos = TryReserveSlots(1);
    if (pos == UINT64_MAX) {
      return 0;
    }
    uint32_t local_seq = AllocLocalSeq();
    entry.seq_ = CompactLogEntry::EncodeSeq(local_seq, 0);
    entry.version_ = NextVersion();
    DramWrite(pos, &entry, 64);
    return entry.seq_;
  }

  uint64_t AppendWithOverflow(CompactLogEntry &entry, const char *overflow_data,
                              int overflow_len) {
    assert(overflow_len > 0);
    int cont_count = (overflow_len + CompactLogOverflow::kDataMax - 1) /
                     CompactLogOverflow::kDataMax;
    assert(cont_count <= kCompactWalMaxContinuationSlots);

    uint64_t pos = TryReserveSlots(1 + cont_count);
    if (pos == UINT64_MAX) {
      return 0;
    }
    uint32_t local_seq = AllocLocalSeq();
    entry.version_ = NextVersion();

    for (int i = cont_count - 1; i >= 0; --i) {
      CompactLogOverflow ov{};
      ov.magic_ = CompactLogOverflow::kOverflowMagic;
      ov.parent_seq_ = local_seq;
      int copy_off = i * CompactLogOverflow::kDataMax;
      int copy_len =
          std::min(overflow_len - copy_off, CompactLogOverflow::kDataMax);
      std::memcpy(ov.data_, overflow_data + copy_off, copy_len);
      DramWrite(pos + 1 + i, &ov, 64);
    }

    entry.seq_ = CompactLogEntry::EncodeSeq(local_seq, cont_count);
    DramWrite(pos, &entry, 64);
    return entry.seq_;
  }

  // Read entry at ring position (for checkpoint/recovery).
  const CompactLogEntry *At(uint64_t pos) const {
    return &ring_[pos & capacity_mask_];
  }

  // Read overflow data at ring position.
  const CompactLogOverflow *OverflowAt(uint64_t pos) const {
    return reinterpret_cast<const CompactLogOverflow *>(
        &ring_[pos & capacity_mask_]);
  }

  void ClearRange(uint64_t begin, uint64_t end) {
    alignas(64) char zero[64] = {};
    for (uint64_t pos = begin; pos < end; ++pos) {
      DramWrite(pos, zero, 64);
    }
  }

  // Advance checkpoint position (called after checkpoint completes).
  void AdvanceCheckpoint(uint64_t new_pos) {
    checkpoint_pos_.store(new_pos, std::memory_order_release);
  }

  uint64_t CheckpointPos() const {
    return checkpoint_pos_.load(std::memory_order_acquire);
  }

  uint64_t Head() const { return head_.load(std::memory_order_acquire); }

  size_t Capacity() const { return capacity_; }

  int MdsId() const { return mds_id_; }

  uint64_t BeginOperation(uint64_t observed_version) {
    uint64_t version = MergeAndTick(observed_version);
    tls_pending_version_owner_ = this;
    tls_pending_version_ = version;
    return version;
  }

  void EndOperation() {
    if (tls_pending_version_owner_ == this) {
      tls_pending_version_owner_ = nullptr;
      tls_pending_version_ = 0;
    }
  }

  void ObserveVersion(uint64_t version) {
    if (version == 0) {
      return;
    }
    const uint64_t observed = version >> CompactLogEntry::kVersionMetaBits;
    uint64_t old = hlc_state_.load(std::memory_order_relaxed);
    while (observed > old && !hlc_state_.compare_exchange_weak(
                                 old, observed, std::memory_order_acq_rel,
                                 std::memory_order_relaxed)) {
    }
  }

  uint64_t CurrentVersion() const {
    if (tls_pending_version_owner_ == this && tls_pending_version_ != 0) {
      return tls_pending_version_;
    }
    uint64_t state = hlc_state_.load(std::memory_order_acquire);
    if (state == 0) {
      return 0;
    }
    uint64_t physical = state >> CompactLogEntry::kVersionLogicalBits;
    uint16_t logical = static_cast<uint16_t>(
        state & ((1u << CompactLogEntry::kVersionLogicalBits) - 1));
    return CompactLogEntry::PackVersion(physical, logical,
                                        static_cast<uint8_t>(mds_id_));
  }

private:
  static uint64_t CurrentPhysicalMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
  }

  uint32_t AllocLocalSeq() {
    while (true) {
      uint32_t seq = local_seq_.fetch_add(1, std::memory_order_relaxed) &
                     CompactLogEntry::kSeqLocalMask;
      if (seq != 0) {
        return seq;
      }
    }
  }

  uint64_t NextVersion() {
    if (tls_pending_version_owner_ == this && tls_pending_version_ != 0) {
      uint64_t version = tls_pending_version_;
      tls_pending_version_ = 0;
      return version;
    }
    return MergeAndTick(0);
  }

  uint64_t MergeAndTick(uint64_t observed_version) {
    while (true) {
      uint64_t old = hlc_state_.load(std::memory_order_relaxed);
      uint64_t old_physical = old >> CompactLogEntry::kVersionLogicalBits;
      uint16_t old_logical = static_cast<uint16_t>(
          old & ((1u << CompactLogEntry::kVersionLogicalBits) - 1));
      uint64_t now_ms = CurrentPhysicalMs();
      uint64_t observed_state =
          observed_version >> CompactLogEntry::kVersionMetaBits;
      uint64_t observed_physical =
          observed_state >> CompactLogEntry::kVersionLogicalBits;
      uint16_t observed_logical = static_cast<uint16_t>(
          observed_state & ((1u << CompactLogEntry::kVersionLogicalBits) - 1));

      uint64_t base_physical = old_physical;
      uint16_t base_logical = old_logical;
      if (observed_physical > base_physical ||
          (observed_physical == base_physical &&
           observed_logical > base_logical)) {
        base_physical = observed_physical;
        base_logical = observed_logical;
      }

      uint64_t next_physical = std::max(now_ms, base_physical);
      uint16_t next_logical = 0;
      if (next_physical == base_physical) {
        if (base_logical ==
            ((1u << CompactLogEntry::kVersionLogicalBits) - 1)) {
          next_physical = base_physical + 1;
          next_logical = 0;
        } else {
          next_logical = base_logical + 1;
        }
      }

      uint64_t next = (next_physical << CompactLogEntry::kVersionLogicalBits) |
                      next_logical;
      if (hlc_state_.compare_exchange_weak(old, next, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
        return CompactLogEntry::PackVersion(next_physical, next_logical,
                                            static_cast<uint8_t>(mds_id_));
      }
    }
  }

  // Write to CXL-SSD DRAM via the device interface (includes delay injection).
  void DramWrite(uint64_t slot_pos, const void *data, uint64_t size) {
    uint64_t offset = ring_dram_offset_ +
                      (slot_pos & capacity_mask_) * sizeof(CompactLogEntry);
    ssd_->DramWrite(offset, size, const_cast<void *>(data));
  }

  CXLSSD *ssd_ = nullptr;
  CompactLogEntry *ring_ = nullptr; // raw pointer for reads
  uint64_t ring_dram_offset_ = 0;   // offset in CXL-SSD DRAM region
  size_t capacity_ = 0;
  size_t capacity_mask_ = 0;
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> checkpoint_pos_{0};
  std::atomic<uint32_t> local_seq_{1};
  std::atomic<uint64_t> hlc_state_{0}; // physical_ms << logical_bits | logical
  int mds_id_ = 0;

  static thread_local CompactWAL *tls_pending_version_owner_;
  static thread_local uint64_t tls_pending_version_;
};

inline thread_local CompactWAL *CompactWAL::tls_pending_version_owner_ =
    nullptr;
inline thread_local uint64_t CompactWAL::tls_pending_version_ = 0;

} // namespace dfs
