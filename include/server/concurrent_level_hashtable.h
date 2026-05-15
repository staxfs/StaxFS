#pragma once

// ConcurrentLevelHashtable — faithful per-slot-locked Level-Hashing baseline.
//
// This file is a deliberate transliteration of the original
// `Level-Hashing/concurrent_level_hashing/level_hashing.{c,h}` algorithm
// (Zuo et al., ATC 2018) onto a CXL-backed memory pool. It is intended as
// the LevelHash evaluation baseline: NO optimizations beyond what is
// strictly required to make the per-slot spinlock protocol work on CXL.
//
// Locking model (matches the reference C code exactly):
//   • One spinlock per slot — there are kAssocNum (=4) locks per bucket.
//   • find / insert / erase / update / try_movement / b2t_movement all
//     iterate slots one at a time, each iteration:
//         spin-lock the slot → check token + key → spin-unlock the slot
//   • No bucket-level dirty bit, no version counter, no fingerprint
//     pre-filter, no ABA-guarded read path. Readers acquire the same
//     spinlock writers do.
//
// CXL realisation of the per-slot spinlock:
//   • `gDevice->CXLAtomicCasSync(offset, expected, desired, &old)` is the
//     only sub-cacheline atomic available. It operates on the 8 bytes at
//     `offset`, but `CXLReadSync` / `CXLWriteSync` enforce 64-byte aligned
//     transfers, so the natural unit of allocation is one 64-byte
//     cacheline per primitive.
//   • Each per-slot lock is therefore a full 64-byte cacheline whose first
//     8 bytes form the CAS target; bit 0 of those 8 bytes is the held flag
//     (0 = free, 1 = held). The remaining 504 bits are padding that
//     prevents false sharing between adjacent slot locks.
//
// Occupancy ("token") storage:
//   • The reference code keeps a separate `uint8_t token[ASSOC_NUM]` array
//     inside each bucket; `token[j] == 1` means slot j is occupied. Each
//     `token[j]` is read fresh under the slot's spinlock on every probe.
//   • Mirroring that, each bucket carries one extra 64-byte cacheline
//     whose first 8 bytes pack four token bytes (byte j == 1 means slot j
//     is occupied). The first 8 bytes are CAS-loop updated when an insert
//     publishes or an erase retires a slot, so two concurrent updates to
//     different bytes are linearised by the CAS rather than the slot lock
//     (the slot lock alone could not protect a sub-byte write on CXL).
//
// Bucket layout on CXL (832 B per bucket = 13 × 64 B cachelines):
//
//     offset    0 ..  63   slot-0 lock word (64 B, bit 0 of first 8 B)
//     offset   64 .. 127   slot-1 lock word
//     offset  128 .. 191   slot-2 lock word
//     offset  192 .. 255   slot-3 lock word
//     offset  256 .. 319   occupancy word  (64 B, 4 token bytes in first 8 B)
//     offset  320 .. 447   slot-0 value    (128 B, holds one V)
//     offset  448 .. 575   slot-1 value
//     offset  576 .. 703   slot-2 value
//     offset  704 .. 831   slot-3 value
//
// Top/bottom two-level layout (TL of N buckets, BL of N/2) and the four
// candidate buckets per key (TL via hash1/hash2, BL via hash1/hash2 mod
// N/2) match the reference code, as do `try_movement` (same-level
// one-hop) and `b2t_movement` (BL→TL promotion) used on insertion overflow.
//
// Resize is stop-the-world, just like `level_resize` in the reference
// code. The only addition is the `CLHResizeMeta` cacheline used to elect
// a single initiator MDS across the disaggregated cluster and to publish
// the new layout to follower MDSs — without this scaffolding the per-MDS
// stop-the-world barrier cannot exist at all in a shared-CXL setting.
// All locking primitives, traversal orders, and movement rules inside the
// algorithm are faithful to the reference C code.
//
// Public surface (constructors, accessors, insert/find/update/erase) is
// signature-compatible with the previous `ConcurrentLevelHashtable` so
// metadata.cc and the test harness compile unchanged. The file is
// self-contained: only `cxl/device.h` + C/C++ stdlib are required.

#include "cxl/device.h"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <spdlog/spdlog.h>

namespace dfs {

// ─── KeyTraits (primary template, specialized in level_hashtable_traits.h) ──
template <typename K, typename V>
struct CLHKeyTraits;

// ─── Resize state machine ─────────────────────────────────────────────
enum class CLHResizeState : uint8_t {
  NORMAL = 0,
  RESIZING = 1,
};

// ─── CLHResizeMeta: 64B CXL shared layout ─────────────────────────────
// Byte layout matches TwoLevelHashtable::ResizeMeta so the same
// pre-allocated CXL slot `DFSHeader::{d,i}_resize_meta_offset_` can host
// either implementation without reallocation.
struct alignas(64) CLHResizeMeta {
  uint64_t ctrl = 0;
  uint64_t version = 0;
  uint64_t tl_offset = 0;
  uint64_t tl_size = 0;
  uint64_t bl_offset = 0;
  uint64_t bl_size = 0;
  uint64_t old_bl_offset = 0;
  uint64_t old_bl_size = 0;
};

static_assert(sizeof(CLHResizeMeta) == 64,
              "CLHResizeMeta must be exactly one CXL cacheline");

// ctrl-word pack/unpack:
//   byte 0: state (CLHResizeState)
//   byte 1: initiator mds_id
//   byte 2: n_mds  (informational)
//   bytes 3..7: reserved
struct CLHResizeCtrl {
  uint64_t raw;

  uint8_t state() const { return raw & 0xFF; }

  uint8_t initiator() const { return (raw >> 8) & 0xFF; }

  uint8_t n_mds() const { return (raw >> 16) & 0xFF; }

  static uint64_t pack(uint8_t st, uint8_t init, uint8_t nm) {
    return (uint64_t)st | ((uint64_t)init << 8) | ((uint64_t)nm << 16);
  }
};

// ─── ConcurrentLevelHashtable ─────────────────────────────────────────
template <typename K, typename V, typename Hash,
          typename KeyTraitsT = CLHKeyTraits<K, V>>
class ConcurrentLevelHashtable {
public:
  // Paper's ASSOC_NUM. Matches `Level-Hashing/.../level_hashing.h`.
  static constexpr int kAssocNum = 4;

  // CXL primitives: CAS targets 8 bytes but reads/writes are 64-B granular,
  // so the per-slot lock word occupies one cacheline. The same constant
  // sizes the per-bucket occupancy ("token") cacheline.
  static constexpr size_t kLockWordSize = 64;
  static constexpr size_t kOccupancyWordSize = 64;

  // Each slot stores one V record contiguously, occupying two cachelines
  // (128 B). This is the same kSlotSize the optimized hashtable uses, so V
  // types defined for either hashtable interoperate.
  static constexpr size_t kSlotSize = 128;
  static constexpr size_t kValueSize = kSlotSize;
  static_assert(kSlotSize % 64 == 0, "slot must be 64-B aligned");
  static_assert(sizeof(V) <= kValueSize, "Value type must fit in 128 B slot");

  // Per-bucket layout (see file header for full diagram).
  static constexpr size_t kLocksOffset = 0;
  static constexpr size_t kOccupancyOffset = kAssocNum * kLockWordSize; // 256
  static constexpr size_t kValuesOffset =
      kOccupancyOffset + kOccupancyWordSize; // 320
  static constexpr size_t kBucketSize =
      kValuesOffset + kAssocNum * kSlotSize; // 832
  static_assert(kBucketSize % 64 == 0, "bucket must be 64-B aligned");

  // Lock-word values. Only bit 0 of the first 8 bytes is used.
  static constexpr uint64_t kLockFree = 0ULL;
  static constexpr uint64_t kLockHeld = 1ULL;

  // Resize at 80% load (matches paper's "near-full" heuristic).
  static constexpr double kResizeThreshold = 0.8;

  // ── Resize configuration. Same shape as TwoLevelHashtable::ResizeConfig
  //    so metadata.cc's brace-init compiles unchanged. `notify_offsets_`
  //    is stored but never read by this baseline (pure CXL polling). ──
  struct CLHResizeConfig {
    uint64_t meta_offset_ = 0;
    int mds_id_ = 0;
    int n_mds_ = 1;
    uint64_t notify_offsets_[kMaxMetaNumOfGroup] = {0}; // unused in baseline

    CLHResizeConfig() = default;

    CLHResizeConfig(uint64_t meta_off, int mds, int n_mds,
                    const uint64_t *offsets)
        : meta_offset_(meta_off), mds_id_(mds), n_mds_(n_mds) {
      if (offsets != nullptr) {
        for (int i = 0; i < n_mds && i < kMaxMetaNumOfGroup; ++i) {
          notify_offsets_[i] = offsets[i];
        }
      }
    }
  };

  // ── Construction ──
  //
  // Leader path: allocate TL/BL buffers in CXL. The caller reads back
  // tl_offset()/bl_offset() and writes the initial CLHResizeMeta.
  ConcurrentLevelHashtable(size_t tl_num_buckets, int n_mds = 1)
      : tl_n_(tl_num_buckets), bl_n_(tl_num_buckets / 2), n_mds_(n_mds) {
    assert(tl_num_buckets >= 2);
    assert((tl_num_buckets & (tl_num_buckets - 1)) == 0);
    assert(gDevice != nullptr);
    tl_offset_.store(gDevice->CXLMemMalloc(tl_n_.load() * kBucketSize),
                     std::memory_order_relaxed);
    bl_offset_.store(gDevice->CXLMemMalloc(bl_n_.load() * kBucketSize),
                     std::memory_order_relaxed);
  }

  // Follower path: attach to an existing CXL allocation.
  ConcurrentLevelHashtable(size_t tl_num_buckets, uint64_t tl_offset,
                           uint64_t bl_offset, int n_mds = 1)
      : tl_offset_(tl_offset), bl_offset_(bl_offset), tl_n_(tl_num_buckets),
        bl_n_(tl_num_buckets / 2), n_mds_(n_mds) {}

  ~ConcurrentLevelHashtable() = default;

  void SetMds(int n_mds, const uint64_t *offsets) {
    n_mds_ = n_mds;
    rcfg_.n_mds_ = n_mds;
    if (offsets != nullptr) {
      for (int i = 0; i < n_mds && i < kMaxMetaNumOfGroup; ++i) {
        rcfg_.notify_offsets_[i] = offsets[i];
      }
    }
  }

  static size_t RequiredMemory(size_t tl_num_buckets) {
    return (tl_num_buckets + tl_num_buckets / 2) * kBucketSize;
  }

  void enable_resize(CLHResizeConfig cfg) {
    rcfg_ = cfg;
    resize_enabled_ = true;
    cached_version_ = read_meta_version();
  }

  // ── Public operations ──

  int insert(const K &key, const V &val) {
    pre_op();
    int r = insert_impl(key, val);
    if (r == 1) {
      post_op();
      return 1;
    }
    if (resize_enabled_) {
      do_resize();
      r = insert_impl(key, val);
    }
    post_op();
    return r;
  }

  bool find(const K &key, V &val) {
    pre_op();
    bool r = find_impl(key, val);
    post_op();
    return r;
  }

  template <typename F>
  bool find(const K &key, F func) {
    V val;
    if (find(key, val)) {
      func(val);
      return true;
    }
    return false;
  }

  bool update(const K &key, const V &val) {
    return update_fn(key, [&](V &cur) { cur = val; });
  }

  template <typename F>
  bool update_fn(const K &key, F func) {
    pre_op();
    bool r = update_impl(key, [&](V &cur) { func(cur); });
    post_op();
    return r;
  }

  bool erase(const K &key) {
    V d;
    return erase(key, d);
  }

  bool erase(const K &key, V &out_val) {
    pre_op();
    bool r = erase_impl(key, out_val);
    post_op();
    return r;
  }

  template <typename F>
  bool erase_fn(const K &key, F func) {
    V val;
    if (erase(key, val)) {
      func(val);
      return true;
    }
    return false;
  }

  // ── Stats / accessors ──
  uint64_t GetElementNum() const {
    return size_.load(std::memory_order_relaxed);
  }

  uint64_t GetSpaceNum() const {
    return (tl_n_.load(std::memory_order_relaxed) +
            bl_n_.load(std::memory_order_relaxed)) *
           kAssocNum;
  }

  double load_factor() const {
    return static_cast<double>(GetElementNum()) / GetSpaceNum();
  }

  double estimated_load_factor() const {
    return static_cast<double>(size_.load(std::memory_order_relaxed) * n_mds_) /
           GetSpaceNum();
  }

  size_t GetSpace() const {
    return (tl_n_.load(std::memory_order_relaxed) +
            bl_n_.load(std::memory_order_relaxed)) *
           kBucketSize;
  }

  size_t tl_num_buckets() const {
    return tl_n_.load(std::memory_order_relaxed);
  }

  size_t bl_num_buckets() const {
    return bl_n_.load(std::memory_order_relaxed);
  }

  uint64_t tl_offset() const {
    return tl_offset_.load(std::memory_order_relaxed);
  }

  uint64_t bl_offset() const {
    return bl_offset_.load(std::memory_order_relaxed);
  }

  bool is_migrating() const {
    return resizing_.load(std::memory_order_acquire);
  }

private:
  // ════════════════════════════════════════════════════════════════════
  // CXL access to the shared CLHResizeMeta
  // ════════════════════════════════════════════════════════════════════

  void read_meta_full(CLHResizeMeta *out) const {
    gDevice->CXLReadSync(rcfg_.meta_offset_, 64, out);
  }

  uint64_t read_meta_ctrl() const {
    alignas(64) CLHResizeMeta m;
    read_meta_full(&m);
    return m.ctrl;
  }

  uint64_t read_meta_version() const {
    alignas(64) CLHResizeMeta m;
    read_meta_full(&m);
    return m.version;
  }

  bool cas_meta_ctrl(uint64_t expected, uint64_t desired) {
    uint64_t old;
    return gDevice->CXLAtomicCasSync(rcfg_.meta_offset_, expected, desired,
                                     &old) == 0;
  }

  // ════════════════════════════════════════════════════════════════════
  // pre_op / post_op — version polling + layout catch-up
  // ════════════════════════════════════════════════════════════════════

  void pre_op() {
    if (!resize_enabled_) {
      return;
    }
    alignas(64) CLHResizeMeta m;
    read_meta_full(&m);
    CLHResizeCtrl ctrl{m.ctrl};
    if (ctrl.state() != (uint8_t)CLHResizeState::NORMAL) {
      catch_up();
      return;
    }
    if (m.version != cached_version_) {
      catch_up();
    }
  }

  void post_op() {}

  void catch_up() {
    for (;;) {
      alignas(64) CLHResizeMeta m;
      read_meta_full(&m);
      CLHResizeCtrl ctrl{m.ctrl};
      if (ctrl.state() == (uint8_t)CLHResizeState::NORMAL) {
        adopt_layout(m);
        return;
      }
      _mm_pause();
    }
  }

  void adopt_layout(const CLHResizeMeta &m) {
    bool exp = false;
    if (!adopting_.compare_exchange_strong(exp, true,
                                           std::memory_order_acq_rel)) {
      while (adopting_.load(std::memory_order_acquire)) {
        _mm_pause();
      }
      return;
    }
    if (m.tl_size > 0) {
      tl_offset_.store(m.tl_offset, std::memory_order_relaxed);
      tl_n_.store(m.tl_size, std::memory_order_relaxed);
      bl_offset_.store(m.bl_offset, std::memory_order_relaxed);
      bl_n_.store(m.bl_size, std::memory_order_relaxed);
    }
    cached_version_ = m.version;
    // Track whether any resize has happened, so b2t_movement is enabled
    // matching the reference code's `level->level_resize` flag.
    level_resize_count_.store(m.version, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    adopting_.store(false, std::memory_order_release);
  }

  // ════════════════════════════════════════════════════════════════════
  // Per-slot offsets and CXL primitives
  // ════════════════════════════════════════════════════════════════════

  static constexpr uint64_t slot_lock_off(uint64_t bucket_off, int slot) {
    return bucket_off + kLocksOffset +
           static_cast<uint64_t>(slot) * kLockWordSize;
  }

  static constexpr uint64_t occupancy_off(uint64_t bucket_off) {
    return bucket_off + kOccupancyOffset;
  }

  static constexpr uint64_t slot_value_off(uint64_t bucket_off, int slot) {
    return bucket_off + kValuesOffset +
           static_cast<uint64_t>(slot) * kSlotSize;
  }

  // ── Spinlock acquire (CAS-loop, blocking — matches level_hashing.c's
  //    xchg-based spin_lock semantics). ──
  static void slot_lock(uint64_t bucket_off, int slot) {
    uint64_t lock_off = slot_lock_off(bucket_off, slot);
    uint64_t old;
    while (gDevice->CXLAtomicCasSync(lock_off, kLockFree, kLockHeld, &old) !=
           0) {
      _mm_pause();
    }
  }

  // Non-blocking variant. Returns true if the lock was acquired.
  // Used only by `try_movement` / `b2t_movement` for the second of two
  // simultaneously-held slot locks, mirroring the reference code's
  // structure that holds the first slot lock while probing alternates
  // (the reference uses blocking spin_lock; we use trylock to avoid
  // cross-MDS deadlock — the strictly-faithful blocking variant is
  // available below as `slot_lock` if the user prefers the textbook
  // semantics with that risk).
  static bool slot_trylock(uint64_t bucket_off, int slot) {
    uint64_t lock_off = slot_lock_off(bucket_off, slot);
    uint64_t old;
    return gDevice->CXLAtomicCasSync(lock_off, kLockFree, kLockHeld, &old) == 0;
  }

  static void slot_unlock(uint64_t bucket_off, int slot) {
    uint64_t lock_off = slot_lock_off(bucket_off, slot);
    uint64_t old;
    int rc =
        gDevice->CXLAtomicCasSync(lock_off, kLockHeld, kLockFree, &old);
    if (rc != 0) {
      SPDLOG_CRITICAL("CLH slot_unlock: CAS failed at lock_off={:#x} "
                      "observed={:#x} — protocol violated",
                      lock_off, old);
    }
  }

  // ── Occupancy word (first 8 bytes hold 4 token bytes, byte j == 1 iff
  //    slot j is occupied — matches `token[ASSOC_NUM]` in the reference). ──
  static uint64_t read_occupancy(uint64_t bucket_off) {
    alignas(64) char buf[64];
    gDevice->CXLReadSync(occupancy_off(bucket_off), 64, buf);
    uint64_t w;
    std::memcpy(&w, buf, sizeof(w));
    return w;
  }

  static bool slot_occupied(uint64_t occ_word, int slot) {
    return ((occ_word >> (slot * 8)) & 0xFFULL) != 0;
  }

  // CAS-loop update of byte `slot` of the occupancy word. Required because
  // two concurrent insertions/erasures on different slots of the same
  // bucket each hold their own slot lock, but both must mutate distinct
  // bytes of the same shared 8-byte word — a plain read-modify-write
  // would race. The CAS linearises them.
  static void set_occupancy(uint64_t bucket_off, int slot, bool occupied) {
    uint64_t off = occupancy_off(bucket_off);
    uint64_t shift = static_cast<uint64_t>(slot) * 8;
    uint64_t mask = 0xFFULL << shift;
    uint64_t set_val = (occupied ? 1ULL : 0ULL) << shift;
    for (;;) {
      uint64_t cur = read_occupancy(bucket_off);
      uint64_t desired = (cur & ~mask) | set_val;
      if (cur == desired) {
        return;
      }
      uint64_t old;
      if (gDevice->CXLAtomicCasSync(off, cur, desired, &old) == 0) {
        return;
      }
    }
  }

  // ── Per-slot value I/O. Each slot is 128 B = 2 cachelines. ──
  static void read_slot_value(uint64_t bucket_off, int slot, V &out) {
    alignas(64) char buf[kSlotSize];
    gDevice->CXLReadSync(slot_value_off(bucket_off, slot), kSlotSize, buf);
    std::memcpy(&out, buf, sizeof(V));
  }

  static void write_slot_value(uint64_t bucket_off, int slot, const V &val) {
    alignas(64) char buf[kSlotSize] = {};
    std::memcpy(buf, &val, sizeof(V));
    gDevice->CXLWriteSync(slot_value_off(bucket_off, slot), kSlotSize, buf);
  }

  // ════════════════════════════════════════════════════════════════════
  // Hash / candidate computation
  // ════════════════════════════════════════════════════════════════════

  static uint64_t secondary_hash(uint64_t h) {
    return h * 0x9E3779B97F4A7C15ULL;
  }

  uint64_t level_offset(int level) const {
    return level == 0 ? tl_offset_.load(std::memory_order_acquire)
                      : bl_offset_.load(std::memory_order_acquire);
  }

  // Mirrors `F_IDX` / `S_IDX` from level_hashing.c: TL uses `addr_capacity`
  // for the modulus (= tl_n_), BL uses `addr_capacity / 2` (= bl_n_).
  size_t f_idx_for_level(uint64_t h1, int level) const {
    return h1 % (level == 0 ? tl_n_.load(std::memory_order_acquire)
                            : bl_n_.load(std::memory_order_acquire));
  }

  size_t s_idx_for_level(uint64_t h2, int level) const {
    return h2 % (level == 0 ? tl_n_.load(std::memory_order_acquire)
                            : bl_n_.load(std::memory_order_acquire));
  }

  // ════════════════════════════════════════════════════════════════════
  // find_impl — direct translation of `level_query`.
  //   for level in {TL, BL}:
  //     for j in 0..ASSOC_NUM:
  //       lock f_idx slot j; check; unlock
  //     for j in 0..ASSOC_NUM:
  //       lock s_idx slot j; check; unlock
  // ════════════════════════════════════════════════════════════════════

  bool find_impl(const K &key, V &val) {
    uint64_t h1 = hasher_(key);
    uint64_t h2 = secondary_hash(h1);

    for (int lvl = 0; lvl < 2; ++lvl) {
      uint64_t base = level_offset(lvl);
      uint64_t f_bucket = base + f_idx_for_level(h1, lvl) * kBucketSize;
      uint64_t s_bucket = base + s_idx_for_level(h2, lvl) * kBucketSize;

      for (int j = 0; j < kAssocNum; ++j) {
        if (probe_slot(f_bucket, j, key, val)) {
          return true;
        }
      }
      for (int j = 0; j < kAssocNum; ++j) {
        if (probe_slot(s_bucket, j, key, val)) {
          return true;
        }
      }
    }
    return false;
  }

  // Lock slot, read occupancy, optionally read value + compare, unlock.
  // Returns true iff the slot held the matching key.
  bool probe_slot(uint64_t bucket_off, int slot, const K &key, V &val) {
    slot_lock(bucket_off, slot);
    uint64_t occ = read_occupancy(bucket_off);
    bool found = false;
    if (slot_occupied(occ, slot)) {
      V candidate;
      read_slot_value(bucket_off, slot, candidate);
      if (key_traits_.match(candidate, key)) {
        val = candidate;
        found = true;
      }
    }
    slot_unlock(bucket_off, slot);
    return found;
  }

  // ════════════════════════════════════════════════════════════════════
  // insert_impl — direct translation of `level_insert`.
  //   pass 1 (interleaved f/s by slot index, matches reference):
  //     for j in 0..ASSOC_NUM:
  //       try f_idx slot j; if placed return
  //       try s_idx slot j; if placed return
  //   then try_movement on each of the 4 candidate buckets
  //   then b2t_movement (only if at least one resize has happened)
  // ════════════════════════════════════════════════════════════════════

  int insert_impl(const K &key, const V &val) {
    uint64_t h1 = hasher_(key);
    uint64_t h2 = secondary_hash(h1);

    for (int lvl = 0; lvl < 2; ++lvl) {
      uint64_t base = level_offset(lvl);
      uint64_t f_bucket = base + f_idx_for_level(h1, lvl) * kBucketSize;
      uint64_t s_bucket = base + s_idx_for_level(h2, lvl) * kBucketSize;
      for (int j = 0; j < kAssocNum; ++j) {
        if (try_insert_slot(f_bucket, j, val)) {
          return 1;
        }
        if (try_insert_slot(s_bucket, j, val)) {
          return 1;
        }
      }
    }

    // Same-level one-hop displacement on each of the 4 candidate buckets.
    for (int lvl = 0; lvl < 2; ++lvl) {
      size_t fi = f_idx_for_level(h1, lvl);
      size_t si = s_idx_for_level(h2, lvl);
      if (try_movement_and_place(lvl, fi, val)) {
        return 1;
      }
      if (try_movement_and_place(lvl, si, val)) {
        return 1;
      }
    }

    // b2t_movement: only legal after at least one resize has happened.
    if (level_resize_count_.load(std::memory_order_acquire) > 0) {
      size_t bl_fi = f_idx_for_level(h1, 1);
      size_t bl_si = s_idx_for_level(h2, 1);
      if (b2t_movement_and_place(bl_fi, val)) {
        return 1;
      }
      if (b2t_movement_and_place(bl_si, val)) {
        return 1;
      }
    }

    return 0;
  }

  // Lock slot, read occupancy, if free: write value + set token, unlock.
  bool try_insert_slot(uint64_t bucket_off, int slot, const V &val) {
    slot_lock(bucket_off, slot);
    uint64_t occ = read_occupancy(bucket_off);
    if (!slot_occupied(occ, slot)) {
      write_slot_value(bucket_off, slot, val);
      set_occupancy(bucket_off, slot, true);
      slot_unlock(bucket_off, slot);
      size_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    slot_unlock(bucket_off, slot);
    return false;
  }

  // Reference `try_movement`:
  //   under lock on src bucket slot i:
  //     compute alternate bucket `jdx` for the existing key
  //     for j in 0..ASSOC_NUM:
  //       lock jdx slot j
  //       if token == 0:
  //         move existing entry to jdx slot j (set token, unlock jdx slot j)
  //         place new entry in src slot i (was occupied; stays occupied)
  //         unlock src slot i
  //         return success
  //       unlock jdx slot j
  //     unlock src slot i
  //   return failure
  bool try_movement_and_place(int level, size_t idx, const V &new_val) {
    uint64_t base = level_offset(level);
    uint64_t src_bucket = base + idx * kBucketSize;
    size_t level_n = (level == 0) ? tl_n_.load(std::memory_order_acquire)
                                  : bl_n_.load(std::memory_order_acquire);

    for (int i = 0; i < kAssocNum; ++i) {
      slot_lock(src_bucket, i);
      uint64_t occ = read_occupancy(src_bucket);
      if (!slot_occupied(occ, i)) {
        // Empty src slot — earlier passes would have already placed our
        // new entry here, but a concurrent erase may have freed it after
        // pass 1. Take advantage and just place.
        write_slot_value(src_bucket, i, new_val);
        set_occupancy(src_bucket, i, true);
        slot_unlock(src_bucket, i);
        size_.fetch_add(1, std::memory_order_relaxed);
        return true;
      }

      V existing;
      read_slot_value(src_bucket, i, existing);
      K existing_key = key_traits_.extract(existing);
      uint64_t eh1 = hasher_(existing_key);
      uint64_t eh2 = secondary_hash(eh1);
      size_t fi = eh1 % level_n;
      size_t si = eh2 % level_n;
      size_t jdx = (idx == fi) ? si : fi;
      if (jdx == idx) {
        // Alternate is the same bucket — same-level move can't help.
        slot_unlock(src_bucket, i);
        continue;
      }

      uint64_t dst_bucket = base + jdx * kBucketSize;
      bool moved = false;
      for (int j = 0; j < kAssocNum; ++j) {
        // Non-blocking acquire of the dst slot to avoid the reference
        // code's theoretical cross-movement deadlock when two threads
        // pick each other as alternates. If we lose the trylock we just
        // skip to the next candidate slot, which is functionally the
        // same as the reference's spin-on-busy except that we never
        // wait on a peer that may be waiting on us.
        if (!slot_trylock(dst_bucket, j)) {
          continue;
        }
        uint64_t dst_occ = read_occupancy(dst_bucket);
        if (slot_occupied(dst_occ, j)) {
          slot_unlock(dst_bucket, j);
          continue;
        }
        // Move existing entry into dst slot.
        write_slot_value(dst_bucket, j, existing);
        set_occupancy(dst_bucket, j, true);
        slot_unlock(dst_bucket, j);
        // Place new entry in vacated src slot (token stays 1).
        write_slot_value(src_bucket, i, new_val);
        slot_unlock(src_bucket, i);
        size_.fetch_add(1, std::memory_order_relaxed);
        moved = true;
        break;
      }
      if (moved) {
        return true;
      }
      slot_unlock(src_bucket, i);
    }
    return false;
  }

  // Reference `b2t_movement`: BL bucket → its two TL candidates.
  bool b2t_movement_and_place(size_t bl_idx, const V &new_val) {
    uint64_t bl_base = bl_offset_.load(std::memory_order_acquire);
    uint64_t tl_base = tl_offset_.load(std::memory_order_acquire);
    size_t tl_n = tl_n_.load(std::memory_order_acquire);
    uint64_t src_bucket = bl_base + bl_idx * kBucketSize;

    for (int i = 0; i < kAssocNum; ++i) {
      slot_lock(src_bucket, i);
      uint64_t occ = read_occupancy(src_bucket);
      if (!slot_occupied(occ, i)) {
        // Empty — place new entry directly.
        write_slot_value(src_bucket, i, new_val);
        set_occupancy(src_bucket, i, true);
        slot_unlock(src_bucket, i);
        size_.fetch_add(1, std::memory_order_relaxed);
        return true;
      }

      V existing;
      read_slot_value(src_bucket, i, existing);
      K existing_key = key_traits_.extract(existing);
      uint64_t eh1 = hasher_(existing_key);
      uint64_t eh2 = secondary_hash(eh1);
      size_t tl_indices[2] = {eh1 % tl_n, eh2 % tl_n};

      bool moved = false;
      for (int ti = 0; ti < 2 && !moved; ++ti) {
        uint64_t dst_bucket = tl_base + tl_indices[ti] * kBucketSize;
        for (int j = 0; j < kAssocNum; ++j) {
          if (!slot_trylock(dst_bucket, j)) {
            continue;
          }
          uint64_t dst_occ = read_occupancy(dst_bucket);
          if (slot_occupied(dst_occ, j)) {
            slot_unlock(dst_bucket, j);
            continue;
          }
          write_slot_value(dst_bucket, j, existing);
          set_occupancy(dst_bucket, j, true);
          slot_unlock(dst_bucket, j);
          write_slot_value(src_bucket, i, new_val);
          slot_unlock(src_bucket, i);
          size_.fetch_add(1, std::memory_order_relaxed);
          moved = true;
          break;
        }
      }
      if (moved) {
        return true;
      }
      slot_unlock(src_bucket, i);
    }
    return false;
  }

  // ════════════════════════════════════════════════════════════════════
  // update_impl — direct translation of `level_update`.
  // Same TL-then-BL, f_idx-all-slots-then-s_idx-all-slots ordering as find.
  // ════════════════════════════════════════════════════════════════════

  template <typename WriteFn>
  bool update_impl(const K &key, WriteFn write_fn) {
    uint64_t h1 = hasher_(key);
    uint64_t h2 = secondary_hash(h1);

    for (int lvl = 0; lvl < 2; ++lvl) {
      uint64_t base = level_offset(lvl);
      uint64_t f_bucket = base + f_idx_for_level(h1, lvl) * kBucketSize;
      uint64_t s_bucket = base + s_idx_for_level(h2, lvl) * kBucketSize;
      for (int j = 0; j < kAssocNum; ++j) {
        if (try_update_slot(f_bucket, j, key, write_fn)) {
          return true;
        }
      }
      for (int j = 0; j < kAssocNum; ++j) {
        if (try_update_slot(s_bucket, j, key, write_fn)) {
          return true;
        }
      }
    }
    return false;
  }

  template <typename WriteFn>
  bool try_update_slot(uint64_t bucket_off, int slot, const K &key,
                       WriteFn &write_fn) {
    slot_lock(bucket_off, slot);
    uint64_t occ = read_occupancy(bucket_off);
    bool updated = false;
    if (slot_occupied(occ, slot)) {
      V cur;
      read_slot_value(bucket_off, slot, cur);
      if (key_traits_.match(cur, key)) {
        write_fn(cur);
        write_slot_value(bucket_off, slot, cur);
        updated = true;
      }
    }
    slot_unlock(bucket_off, slot);
    return updated;
  }

  // ════════════════════════════════════════════════════════════════════
  // erase_impl — direct translation of `level_delete`.
  // ════════════════════════════════════════════════════════════════════

  bool erase_impl(const K &key, V &out_val) {
    uint64_t h1 = hasher_(key);
    uint64_t h2 = secondary_hash(h1);

    for (int lvl = 0; lvl < 2; ++lvl) {
      uint64_t base = level_offset(lvl);
      uint64_t f_bucket = base + f_idx_for_level(h1, lvl) * kBucketSize;
      uint64_t s_bucket = base + s_idx_for_level(h2, lvl) * kBucketSize;
      for (int j = 0; j < kAssocNum; ++j) {
        if (try_erase_slot(f_bucket, j, key, out_val)) {
          return true;
        }
      }
      for (int j = 0; j < kAssocNum; ++j) {
        if (try_erase_slot(s_bucket, j, key, out_val)) {
          return true;
        }
      }
    }
    return false;
  }

  bool try_erase_slot(uint64_t bucket_off, int slot, const K &key, V &out_val) {
    slot_lock(bucket_off, slot);
    uint64_t occ = read_occupancy(bucket_off);
    bool erased = false;
    if (slot_occupied(occ, slot)) {
      V cur;
      read_slot_value(bucket_off, slot, cur);
      if (key_traits_.match(cur, key)) {
        out_val = cur;
        set_occupancy(bucket_off, slot, false);
        erased = true;
      }
    }
    slot_unlock(bucket_off, slot);
    if (erased) {
      size_.fetch_sub(1, std::memory_order_relaxed);
    }
    return erased;
  }

  // ════════════════════════════════════════════════════════════════════
  // Resize (stop-the-world, multi-MDS coordinated via CLHResizeMeta).
  // Mirrors `level_resize` in the reference code: the initiator scans
  // every bucket of old_BL, copies each occupied slot into new_TL, then
  // publishes the new layout. Followers block in pre_op().
  // ════════════════════════════════════════════════════════════════════

  void do_resize() {
    uint64_t cur_ver = read_meta_version();
    if (cur_ver != cached_version_) {
      catch_up();
      return;
    }
    if (!try_initiate_resize()) {
      wait_for_resize_to_finish();
    }
  }

  bool try_initiate_resize() {
    alignas(64) CLHResizeMeta m;
    read_meta_full(&m);
    CLHResizeCtrl ctrl{m.ctrl};
    if (ctrl.state() != (uint8_t)CLHResizeState::NORMAL) {
      return false;
    }
    uint64_t new_ctrl =
        (m.ctrl & ~0xFFULL) | (uint64_t)CLHResizeState::RESIZING;
    if (!cas_meta_ctrl(m.ctrl, new_ctrl)) {
      return false;
    }
    if (m.tl_size > 0) {
      tl_offset_.store(m.tl_offset, std::memory_order_relaxed);
      tl_n_.store(m.tl_size, std::memory_order_relaxed);
      bl_offset_.store(m.bl_offset, std::memory_order_relaxed);
      bl_n_.store(m.bl_size, std::memory_order_relaxed);
    }
    resizing_.store(true, std::memory_order_release);
    run_resize_as_initiator(m);
    resizing_.store(false, std::memory_order_release);
    return true;
  }

  void run_resize_as_initiator(const CLHResizeMeta &prev_meta) {
    SPDLOG_INFO("CLH resize initiator: load={:.2f} tl={} bl={}",
                estimated_load_factor(), tl_num_buckets(), bl_num_buckets());

    size_t old_tl_n = tl_n_.load(std::memory_order_acquire);
    uint64_t old_tl_off = tl_offset_.load(std::memory_order_acquire);
    size_t old_bl_n = bl_n_.load(std::memory_order_acquire);
    uint64_t old_bl_off = bl_offset_.load(std::memory_order_acquire);

    // new_BL inherits old_TL verbatim (h % old_tl_n == h % new_bl_n).
    size_t new_bl_n = old_tl_n;
    uint64_t new_bl_off = old_tl_off;

    // Reference Level-Hashing gives each old_BL entry only 2 destination
    // buckets in new_TL with no displacement. Pathological hashes can
    // therefore overflow even at low load — on overflow we discard,
    // double new_tl_n, and rerun, mirroring §V.B of the paper.
    static constexpr int kRehashMaxAttempts = 4;
    size_t new_tl_n = old_tl_n * 2;
    uint64_t new_tl_off = 0;
    bool rehash_ok = false;

    for (int attempt = 0; attempt < kRehashMaxAttempts; ++attempt) {
      new_tl_off = gDevice->CXLMemMalloc(new_tl_n * kBucketSize);
      if (new_tl_off == 0) {
        SPDLOG_ERROR("CLH resize: CXLMemMalloc({}) failed",
                     new_tl_n * kBucketSize);
        std::abort();
      }
      bool overflow = false;
      for (size_t i = 0; i < old_bl_n && !overflow; ++i) {
        uint64_t bucket_off = old_bl_off + i * kBucketSize;
        uint64_t occ = read_occupancy(bucket_off);
        for (int s = 0; s < kAssocNum; ++s) {
          if (!slot_occupied(occ, s)) {
            continue;
          }
          V val;
          read_slot_value(bucket_off, s, val);
          K key = key_traits_.extract(val);
          uint64_t eh1 = hasher_(key);
          uint64_t eh2 = secondary_hash(eh1);
          size_t fi = eh1 % new_tl_n;
          size_t si = eh2 % new_tl_n;
          if (!seed_into_new_tl(new_tl_off, fi, val) &&
              !seed_into_new_tl(new_tl_off, si, val)) {
            overflow = true;
            break;
          }
        }
      }

      if (!overflow) {
        rehash_ok = true;
        break;
      }

      SPDLOG_WARN("CLH resize: new_TL ({} buckets) overflowed during rehash; "
                  "doubling and retrying",
                  new_tl_n);
      gDevice->CXLMemFree(new_tl_off, new_tl_n * kBucketSize);
      new_tl_off = 0;
      new_tl_n *= 2;
    }

    if (!rehash_ok) {
      SPDLOG_ERROR(
          "CLH resize: rehash still overflows after {} attempts (final new_TL "
          "={} buckets); workload has pathological hash collisions",
          kRehashMaxAttempts, new_tl_n);
      std::abort();
    }

    // Defer free of the PREVIOUS round's old_BL by one resize round.
    if (prev_meta.old_bl_size > 0) {
      gDevice->CXLMemFree(prev_meta.old_bl_offset,
                          prev_meta.old_bl_size * kBucketSize);
    }

    alignas(64) CLHResizeMeta out{};
    out.ctrl =
        CLHResizeCtrl::pack((uint8_t)CLHResizeState::NORMAL,
                            (uint8_t)rcfg_.mds_id_, (uint8_t)rcfg_.n_mds_);
    out.version = prev_meta.version + 1;
    out.tl_offset = new_tl_off;
    out.tl_size = new_tl_n;
    out.bl_offset = new_bl_off;
    out.bl_size = new_bl_n;
    out.old_bl_offset = old_bl_off;
    out.old_bl_size = old_bl_n;
    _mm_sfence();
    gDevice->CXLWriteSync(rcfg_.meta_offset_, 64, &out);

    tl_offset_.store(new_tl_off, std::memory_order_relaxed);
    tl_n_.store(new_tl_n, std::memory_order_relaxed);
    bl_offset_.store(new_bl_off, std::memory_order_relaxed);
    bl_n_.store(new_bl_n, std::memory_order_relaxed);
    cached_version_ = out.version;
    level_resize_count_.fetch_add(1, std::memory_order_acq_rel);
  }

  // Single-writer rehash placement. No locking — only the initiator
  // writes to new_TL while other MDSs are blocked in pre_op().
  bool seed_into_new_tl(uint64_t new_tl_off, size_t bi, const V &val) {
    uint64_t bucket_off = new_tl_off + bi * kBucketSize;
    uint64_t occ = read_occupancy(bucket_off);
    int target = -1;
    for (int s = 0; s < kAssocNum; ++s) {
      if (!slot_occupied(occ, s)) {
        target = s;
        break;
      }
    }
    if (target < 0) {
      return false;
    }
    write_slot_value(bucket_off, target, val);
    set_occupancy(bucket_off, target, true);
    return true;
  }

  void wait_for_resize_to_finish() { catch_up(); }

  // ════════════════════════════════════════════════════════════════════
  // Members
  // ════════════════════════════════════════════════════════════════════

  std::atomic<uint64_t> tl_offset_{0};
  std::atomic<uint64_t> bl_offset_{0};
  std::atomic<size_t> tl_n_;
  std::atomic<size_t> bl_n_;
  std::atomic<bool> adopting_{false};
  std::atomic<bool> resizing_{false};

  int n_mds_ = 1;
  mutable std::atomic<uint64_t> size_{0};
  std::atomic<uint64_t> level_resize_count_{0};
  Hash hasher_;
  KeyTraitsT key_traits_;

  bool resize_enabled_ = false;
  uint64_t cached_version_ = 0;
  CLHResizeConfig rcfg_{};
};

} // namespace dfs
