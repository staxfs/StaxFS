#pragma once

// InodeArray: direct-mapped inode storage on CXL memory.
//
// Replaces TwoLevelHashtable for inode storage. Since inode IDs are
// monotonically increasing per meta and uniquely identify inodes,
// hashing is unnecessary — we use a three-dimensional array:
//   InodeArray[meta_num][segment_idx][slot_offset]
//
// Each inode occupies a 128-byte InodeSlot (2 CXL cachelines).
// Segments are 128MB each, allocated on demand.
// Each meta has a fixed InodeArrayBlock on CXL containing a header
// and up to kMaxSegments segment offset entries.

#include "common/metadata_types.h"
#include "cxl/device.h"
#include "spdlog/spdlog.h"
#include <atomic>
#include <cstring>
#include <mutex>

namespace dfs {

// ── Array dimension constants ──

static constexpr int kSlotBits = 20;
static constexpr size_t kSlotsPerSegment = 1ULL << kSlotBits; // 1,048,576
static constexpr size_t kSlotMask = kSlotsPerSegment - 1;     // 0xFFFFF
static constexpr size_t kSlotSize = 128;                      // bytes per slot
static constexpr size_t kSegmentSize =
    kSlotsPerSegment * kSlotSize; // 128 MB per segment
static constexpr size_t kMaxSegments = 1024;
static constexpr size_t kIAHeaderSize = 64; // 1 cacheline
static constexpr size_t kBlockSize =
    kIAHeaderSize + kMaxSegments * sizeof(uint64_t); // ~8.1KB

// ── CXL InodeArrayBlock header (first 64 bytes) ──

static constexpr uint64_t kExpandingBit = 1ULL << 63;

struct alignas(64) InodeArrayHeader {
  uint64_t ctrl;         // [bit63]=EXPANDING, rest reserved
  uint64_t num_segments; // number of allocated segments
  uint64_t base_id;      // this meta's base inode ID
  uint64_t meta_id;      // this meta's ID
  uint64_t version;      // bumped on each segment allocation
  char reserved[24];     // pad1 to 64B
};

static_assert(sizeof(InodeArrayHeader) == 64);

// ── InodeSlot: 128-byte CXL slot (2 cachelines) ──
// Layout:
//   CL0 [0..63]:  control_(8B) + inode bytes 0-55 (id_ .. blksize_)
//   CL1 [64..127]: inode bytes 56-87 (blocks_ .. ctime_) + reserved_(32B)
//
// The split exactly at byte 56 of Inode depends on sizeof(Inode)==88;
// adding fields silently changes the cacheline boundary and corrupts
// the wire format.
static_assert(sizeof(Inode) == 88, "InodeSlot layout assumes sizeof(Inode)==88 "
                                   "(56B in CL0 + 32B in CL1)");

struct alignas(128) InodeSlot {
  uint64_t control_; // D-bit(1) + version(7) + reserved(55) + valid(1)
  Inode inode_;      // 88 bytes
  char reserved_[32];

  // Control word bit layout
  static constexpr uint64_t kDirtyBit = 1ULL << 63;
  static constexpr int kVersionShift = 56;
  static constexpr uint64_t kVersionMask = 0x7FULL << kVersionShift;
  static constexpr uint64_t kValidBit = 1ULL;

  static auto MakeLocked(uint64_t ctrl) -> uint64_t { return ctrl | kDirtyBit; }

  static auto MakeUnlockedWithVersionBump(uint64_t old_ctrl) -> uint64_t {
    uint64_t ver = ((old_ctrl >> kVersionShift) & 0x7F) + 1;
    uint64_t cleared = old_ctrl & ~(kDirtyBit | kVersionMask);
    return cleared | ((ver & 0x7F) << kVersionShift);
  }
};

static_assert(sizeof(InodeSlot) == 128);

class InodeArray {
public:
  using K = uint64_t;
  using V = Inode;

  InodeArray(int meta_id, int n_mds, uint64_t base_id)
      : meta_id_(meta_id), n_mds_(n_mds), base_id_(base_id),
        local_num_segments_(0) {
    for (int m = 0; m < kMaxMetaNumOfGroup; ++m) {
      for (size_t s = 0; s < kMaxSegments; ++s) {
        seg_offsets_[m][s].store(0, std::memory_order_relaxed);
      }
    }
    std::memset(block_offs_, 0, sizeof(block_offs_));
    InitBlock();
  }

  auto insert(const K &key, const V &val) -> int {
    uint64_t local_id = key - base_id_;
    uint64_t seg_idx = local_id >> kSlotBits;
    uint64_t slot_off = local_id & kSlotMask;

    EnsureSegment(seg_idx);
    uint64_t offset =
        seg_offsets_[meta_id_][seg_idx].load(std::memory_order_relaxed) +
        slot_off * kSlotSize;

    alignas(128) char slot_buf[128] = {};
    uint64_t ctrl = InodeSlot::kValidBit; // D-bit=0, version=0, valid=1
    std::memcpy(slot_buf, &ctrl, 8);
    std::memcpy(slot_buf + 8, &val, sizeof(Inode));
    gDevice->CXLWriteSync(offset, 128, slot_buf);

    return 1;
  }

  auto find(const K &key, V &val) -> bool {
    uint64_t offset = ResolveSlot(key);
    if (offset == 0) {
      return false;
    }

    alignas(64) char verify_buf[64];
    for (;;) {
      gDevice->CXLReadSync(offset, 64, verify_buf);
      uint64_t ctrl_before;
      std::memcpy(&ctrl_before, verify_buf, 8);

      if (ctrl_before & InodeSlot::kDirtyBit) {
        continue; // spin until writer releases
      }
      if (!(ctrl_before & InodeSlot::kValidBit)) {
        return false; // slot not initialized
      }

      // Re-read to verify version consistency
      alignas(64) char buf[128];
      gDevice->CXLReadSync(offset, 128, buf);
      uint64_t ctrl_after;
      std::memcpy(&ctrl_after, buf, 8);

      if (ctrl_after != ctrl_before) {
        continue; // concurrent write detected, retry
      }

      std::memcpy(&val, buf + 8, sizeof(Inode));
      return true;
    }
  }

  template <typename F>
  auto find(const K &key, F func) -> bool {
    V val;
    if (find(key, val)) {
      func(val);
      return true;
    }
    return false;
  }

  auto update(const K &key, const V &val) -> bool {
    return update_fn(key, [&](V &cur) { cur = val; });
  }

  template <typename F>
  auto update_fn(const K &key, F func) -> bool {
    uint64_t offset = ResolveSlot(key);
    if (offset == 0) {
      return false;
    }

    alignas(64) char cl0_buf[64];
    gDevice->CXLReadSync(offset, 64, cl0_buf);
    uint64_t old_ctrl;
    std::memcpy(&old_ctrl, cl0_buf, 8);

    for (;;) {
      while ((old_ctrl & InodeSlot::kDirtyBit) != 0U) {
        gDevice->CXLReadSync(offset, 64, cl0_buf);
        std::memcpy(&old_ctrl, cl0_buf, 8);
      }
      if ((old_ctrl & InodeSlot::kValidBit) == 0U) {
        return false;
      }

      uint64_t new_ctrl = InodeSlot::MakeLocked(old_ctrl);
      uint64_t cas_result;
      gDevice->CXLAtomicCasSync(offset, old_ctrl, new_ctrl, &cas_result);
      if (cas_result == old_ctrl) {
        break;
      }
      old_ctrl = cas_result; // free fresh ctrl from CAS
    }

    alignas(128) char slot_buf[128];
    gDevice->CXLReadSync(offset, 128, slot_buf);

    V inode;
    std::memcpy(&inode, slot_buf + 8, sizeof(Inode));

    func(inode);

    uint64_t final_ctrl =
        InodeSlot::MakeUnlockedWithVersionBump(old_ctrl) | InodeSlot::kValidBit;
    std::memcpy(slot_buf, &final_ctrl, 8);
    std::memcpy(slot_buf + 8, &inode, sizeof(Inode));
    gDevice->CXLWriteSync(offset, 128, slot_buf);

    return true;
  }

  auto erase(const K &key) -> bool {
    uint64_t offset = ResolveSlot(key);
    if (offset == 0) {
      return false;
    }

    alignas(64) char cl0_buf[64];
    gDevice->CXLReadSync(offset, 64, cl0_buf);
    uint64_t old_ctrl;
    std::memcpy(&old_ctrl, cl0_buf, 8);

    for (;;) {
      while ((old_ctrl & InodeSlot::kDirtyBit) != 0U) {
        gDevice->CXLReadSync(offset, 64, cl0_buf);
        std::memcpy(&old_ctrl, cl0_buf, 8);
      }
      if ((old_ctrl & InodeSlot::kValidBit) == 0U) {
        return false;
      }

      uint64_t new_ctrl = InodeSlot::MakeLocked(old_ctrl);
      uint64_t cas_result;
      gDevice->CXLAtomicCasSync(offset, old_ctrl, new_ctrl, &cas_result);
      if (cas_result == old_ctrl) {
        break;
      }
      old_ctrl = cas_result;
    }

    // ID is not reused, so version is unused after erase. Just zero CL0.
    std::memset(cl0_buf, 0, 64);
    gDevice->CXLWriteSync(offset, 64, cl0_buf);

    return true;
  }

  auto erase(const K &key, V &out_val) -> bool {
    uint64_t offset = ResolveSlot(key);
    if (offset == 0) {
      return false;
    }

    alignas(64) char cl0_buf[64];
    gDevice->CXLReadSync(offset, 64, cl0_buf);
    uint64_t old_ctrl;
    std::memcpy(&old_ctrl, cl0_buf, 8);

    for (;;) {
      while ((old_ctrl & InodeSlot::kDirtyBit) != 0U) {
        gDevice->CXLReadSync(offset, 64, cl0_buf);
        std::memcpy(&old_ctrl, cl0_buf, 8);
      }
      if ((old_ctrl & InodeSlot::kValidBit) == 0U) {
        return false;
      }

      uint64_t new_ctrl = InodeSlot::MakeLocked(old_ctrl);
      uint64_t cas_result;
      gDevice->CXLAtomicCasSync(offset, old_ctrl, new_ctrl, &cas_result);
      if (cas_result == old_ctrl) {
        break;
      }
      old_ctrl = cas_result;
    }

    alignas(128) char slot_buf[128];
    gDevice->CXLReadSync(offset, 128, slot_buf);
    std::memcpy(&out_val, slot_buf + 8, sizeof(Inode));

    std::memset(slot_buf, 0, 64);
    gDevice->CXLWriteSync(offset, 64, slot_buf);

    return true;
  }

  template <typename F>
  auto erase_fn(const K &key, F func) -> bool {
    V val;
    if (erase(key, val)) {
      func(val);
      return true;
    }
    return false;
  }

  auto GetSpace() const -> size_t {
    int segmemts = 0;
    for (int i = 0; i < n_mds_; i++) {
      for (int j = 0; j < kMaxSegments; j++) {
        if (seg_offsets_[i][j] != 0) {
          segmemts++;
        }
      }
    }
    return segmemts * kSegmentSize + kBlockSize * n_mds_;
  }

  auto NumAllocatedSegments() const -> size_t {
    return local_num_segments_.load(std::memory_order_relaxed);
  }

  void SetMds(int n_mds) {
    auto *dfs_hdr = gDevice->dfs_header_;
    for (int i = 0; i < n_mds; i++) {
      block_offs_[i] = dfs_hdr->inode_array_block_offset_[i];
    }
    n_mds_ = n_mds;
  }

private:
  void InitBlock() {
    uint64_t block_off = gDevice->CXLMemMalloc(kBlockSize);
    block_offs_[meta_id_] = block_off;

    uint64_t seg0_off = gDevice->CXLMemMalloc(kSegmentSize);
    alignas(64) char cl_buf[64] = {};
    std::memcpy(cl_buf, &seg0_off, 8);
    gDevice->CXLWriteSync(block_off + kIAHeaderSize, 64, cl_buf);

    InodeArrayHeader hdr = {};
    hdr.ctrl = 0;
    hdr.num_segments = 1;
    hdr.base_id = base_id_;
    hdr.meta_id = meta_id_;
    hdr.version = 1;
    gDevice->CXLWriteSync(block_off, 64, &hdr);

    gDevice->dfs_header_->inode_array_block_offset_[meta_id_] = block_off;

    seg_offsets_[meta_id_][0].store(seg0_off, std::memory_order_relaxed);
    local_num_segments_.store(1, std::memory_order_relaxed);

    SPDLOG_INFO("InodeArray: meta {} initialized block at CXL offset {:#x}, "
                "seg0 at {:#x}, base_id={:#x}",
                meta_id_, block_off, seg0_off, base_id_);
  }

  auto ResolveSlot(uint64_t inode_id) -> uint64_t {
    int meta = inode_id >> INODE_ID_RANGE;
    uint64_t base = static_cast<uint64_t>(meta) << INODE_ID_RANGE;
    uint64_t local_id = inode_id - base;
    uint64_t seg_idx = local_id >> kSlotBits;
    uint64_t slot_off = local_id & kSlotMask;

    if (meta >= kMaxMetaNumOfGroup || seg_idx >= kMaxSegments) {
      return 0;
    }

    // Hot path: pure DRAM lookup.
    uint64_t seg_off =
        seg_offsets_[meta][seg_idx].load(std::memory_order_relaxed);
    if (seg_off != 0) {
      return seg_off + slot_off * kSlotSize;
    }

    // Cold-miss path:
    //   - Local meta with cache miss means the segment was never allocated.
    //   - Foreign meta with cache miss: pull from CXL, update cache.
    if (meta == meta_id_) {
      return 0;
    }
    seg_off = FetchForeignSegOffset(meta, seg_idx);
    if (seg_off == 0) {
      return 0;
    }
    return seg_off + slot_off * kSlotSize;
  }

  // Read a foreign meta's seg_offset from CXL and update the local cache.
  auto FetchForeignSegOffset(int target_meta, uint64_t seg_idx) -> uint64_t {
    uint64_t target_block_off = block_offs_[target_meta];
    if (target_block_off == 0) {
      return 0; // target meta not discovered
    }

    alignas(64) InodeArrayHeader hdr;
    gDevice->CXLReadSync(target_block_off, 64, &hdr);

    while (hdr.ctrl & kExpandingBit) {
      gDevice->CXLReadSync(target_block_off, 64, &hdr);
    }

    if (seg_idx >= hdr.num_segments) {
      return 0; // segment not allocated yet
    }

    // Read the cacheline containing seg_offsets[seg_idx] and update
    // local cache for ALL 8 entries it carries.
    uint64_t offsets_base = target_block_off + kIAHeaderSize;
    uint64_t cacheline_idx = seg_idx / 8;
    uint64_t cacheline_off = offsets_base + cacheline_idx * 64;
    uint64_t slot_in_cl = seg_idx % 8;

    alignas(64) uint64_t cl_buf[8];
    gDevice->CXLReadSync(cacheline_off, 64, cl_buf);
    for (int i = 0; i < 8; ++i) {
      uint64_t idx = cacheline_idx * 8 + i;
      if (idx < kMaxSegments && cl_buf[i] != 0) {
        seg_offsets_[target_meta][idx].store(cl_buf[i],
                                             std::memory_order_relaxed);
      }
    }
    return cl_buf[slot_in_cl];
  }

  void EnsureSegment(size_t seg_idx) {
    // Fast path: already allocated (DRAM-only check)
    if (seg_offsets_[meta_id_][seg_idx].load(std::memory_order_relaxed) != 0) {
      return;
    }

    // Slow path: lock + allocate
    std::lock_guard<std::mutex> lock(expand_mu_);

    // Double-check under lock
    if (seg_offsets_[meta_id_][seg_idx].load(std::memory_order_relaxed) != 0) {
      return;
    }

    if (seg_idx >= kMaxSegments) {
      SPDLOG_ERROR("InodeArray: segment index {} exceeds kMaxSegments {}",
                   seg_idx, kMaxSegments);
      return;
    }

    uint64_t block_off = block_offs_[meta_id_];

    // CAS on block header ctrl to set EXPANDING. We hold expand_mu_ so no
    // other thread on this meta can race; the only way the CAS fails is if
    // a prior crash left EXPANDING residue. Force-clear and retry; the
    // retry must succeed since no peer ever writes our header.
    uint64_t cas_result;
    gDevice->CXLAtomicCasSync(block_off, 0ULL, kExpandingBit, &cas_result);
    if (cas_result != 0ULL) {
      SPDLOG_WARN("InodeArray: EXPANDING bit was set (crash residue?), "
                  "forcing clear");
      alignas(64) InodeArrayHeader hdr;
      gDevice->CXLReadSync(block_off, 64, &hdr);
      hdr.ctrl = 0;
      gDevice->CXLWriteSync(block_off, 64, &hdr);
      gDevice->CXLAtomicCasSync(block_off, 0ULL, kExpandingBit, &cas_result);
      if (cas_result != 0ULL) {
        SPDLOG_ERROR("InodeArray: meta {} failed to acquire EXPANDING after "
                     "force-clear (header ctrl={:#x}); aborting allocation "
                     "of seg {} to avoid breaking foreign reader spin-wait",
                     meta_id_, cas_result, seg_idx);
        return;
      }
    }

    // Allocate 128MB segment
    uint64_t new_off = gDevice->CXLMemMalloc(kSegmentSize);

    SPDLOG_INFO("InodeArray: meta {} allocated segment {}", meta_id_, seg_idx);

    // Write seg_offsets[seg_idx] into the block (read-modify-write cacheline)
    uint64_t offsets_base = block_off + kIAHeaderSize;
    uint64_t cacheline_idx = seg_idx / 8;
    uint64_t cacheline_off = offsets_base + cacheline_idx * 64;
    uint64_t slot_in_cl = seg_idx % 8;

    alignas(64) uint64_t cl_buf[8];
    gDevice->CXLReadSync(cacheline_off, 64, cl_buf);
    cl_buf[slot_in_cl] = new_off;
    gDevice->CXLWriteSync(cacheline_off, 64, cl_buf);

    alignas(64) InodeArrayHeader hdr;
    gDevice->CXLReadSync(block_off, 64, &hdr);
    hdr.ctrl = 0;
    if (seg_idx + 1 > hdr.num_segments) {
      hdr.num_segments = seg_idx + 1;
    }
    hdr.version++;
    gDevice->CXLWriteSync(block_off, 64, &hdr);

    // Update local DRAM state
    seg_offsets_[meta_id_][seg_idx].store(new_off, std::memory_order_relaxed);
    size_t cur = local_num_segments_.load(std::memory_order_relaxed);
    if (seg_idx + 1 > cur) {
      local_num_segments_.store(seg_idx + 1, std::memory_order_relaxed);
    }
  }

  int meta_id_;
  int n_mds_;
  uint64_t base_id_;

  // Number of segments allocated for THIS meta.
  std::atomic<size_t> local_num_segments_;

  std::mutex expand_mu_;

  // CXL block offsets for every meta in the group.
  uint64_t block_offs_[kMaxMetaNumOfGroup];

  // Cached seg_offsets for ALL metas: seg_offsets_[meta][seg_idx].
  std::atomic<uint64_t> seg_offsets_[kMaxMetaNumOfGroup][kMaxSegments];
};

} // namespace dfs
