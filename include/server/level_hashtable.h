#pragma once

// Two-Level Hash Table with Bucket-Level D-bit + Version Protocol
//
// Structure: TL(N) + BL(N/2), 8 slots/bucket, 4 candidate buckets/key
// Bucket: 64B hash region (8×8B slot_meta) + 8×128B value region = 1088B
//
// Hash region layout:
//   meta[0] (8B): bit[63]=D (lock), bit[62:60]=version(3b), bit[59:0]=fp(60b)
//   meta[1..7] (8B each): bit[59:0]=fp (60b), upper bits unused (0)
//   fp==0 ⟹ empty slot; fp remapped from 0→1
//
// Write protocol (Insert/Update/Delete/Migration):
//   1. ReadSync(hr, 64)  → initial scan, get meta[0] for CAS
//   2. CAS(hr_off, meta[0], make_locked(meta[0])) → lock (D=1, ver+1)
//      Version increment in CAS prevents ABA → initial read stays valid
//   3. Operate on values (no re-read needed)
//   4. WriteSync(hr, 64, new_keys) → atomic commit + unlock (D=0, ver+1)
//
// Read protocol (Find):
//   1. ReadSync(hr, 64)  → if D=1, defer to pass 2
//   2. ReadSync(v, 128)  → read value
//   3. ReadSync(hr, 64)  → verify meta[0] unchanged (version detects any write)
//
// Progressive resize (built-in, multi-MDS, CXL-native):
//   States: NORMAL → MIGRATING → NORMAL
//   ResizeMeta on CXL shared memory, accessed via CXL Read/Write/CAS
//   GIM notifications for cross-MDS coordination
//   Thread-safe: atomic cursor, atomic migration flags

#include "cxl/device.h"
#include "server/hashtable_stats.h"
#ifdef LEVEL_HASHTABLE_TAG_HINT
  #include "server/tag_hint.h"
#endif
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <spdlog/spdlog.h>

namespace dfs {

// ─── Key Traits ─────────────────────────────────────────────────────
template <typename K, typename V>
struct KeyTraits;

// ─── Resize States ──────────────────────────────────────────────────
enum class ResizeState : uint8_t {
  NORMAL = 0,
  MIGRATING = 1,
};

// ─── ResizeMeta: CXL shared memory layout (64B) ────────────────────
// All runtime access must go through CXL operations, never direct pointer.
// Only used for sizeof() during allocation for init.
//
// ctrl word: [7:0]=state [15:8]=initiator [23:16]=n_mds_ [31:24]=done_count
struct alignas(64) ResizeMeta {
  uint64_t ctrl = 0;
  uint64_t version = 0;
  uint64_t tl_offset = 0;
  uint64_t tl_size = 0;
  uint64_t bl_offset = 0;
  uint64_t bl_size = 0;
  uint64_t old_bl_offset = 0;
  uint64_t old_bl_size = 0;
};

static_assert(sizeof(ResizeMeta) == 64);

// ctrl word pack/unpack helpers
struct ResizeCtrl {
  uint64_t raw;

  uint8_t state() const { return raw & 0xFF; }

  uint8_t initiator() const { return (raw >> 8) & 0xFF; }

  uint8_t n_mds_() const { return (raw >> 16) & 0xFF; }

  uint8_t done_count() const { return (raw >> 24) & 0xFF; }

  static uint64_t pack(uint8_t st, uint8_t init, uint8_t nm, uint8_t dc) {
    return (uint64_t)st | ((uint64_t)init << 8) | ((uint64_t)nm << 16) |
           ((uint64_t)dc << 24);
  }
};

// ─── Local Resize Cache (per-MDS, DRAM, thread-safe) ────────────────
struct LocalResizeCache {
  ResizeState state = ResizeState::NORMAL;
  uint64_t tl_offset = 0;
  size_t tl_size = 0;
  uint64_t bl_offset = 0;
  size_t bl_size = 0;
  uint64_t old_bl_offset = 0;
  size_t old_bl_size = 0;
  uint64_t version = 0;
  size_t range_end = 0;
  std::atomic<size_t> local_cursor{0};
  std::atomic<bool> range_done{true};
  std::atomic<bool> done_reported{false}; // one-shot done_count increment
};

// ─── Two-Level Hash Table ───────────────────────────────────────────

template <typename K, typename V, typename Hash,
          typename KeyTraitsT = KeyTraits<K, V>>
class TwoLevelHashtable {
public:
  static constexpr uint64_t kDirtyBit = 1ULL << 63;
  static constexpr int kVersionShift = 60;
  static constexpr uint64_t kFpMask = (1ULL << 60) - 1;
  static constexpr int kSlots = 8;
  static constexpr size_t kValueSize = 128;
  static constexpr size_t kHashRegionSize = kSlots * sizeof(uint64_t); // 64B
  static constexpr size_t kBucketSize =
      kHashRegionSize + kSlots * kValueSize; // 1088B
  static_assert(sizeof(V) <= kValueSize, "Value type must fit in 128B slot");
  static constexpr int kEpochRetryMax = 1;

  // ── Resize configuration (data only, no callbacks) ──
  struct ResizeConfig {
    uint64_t meta_offset_ = 0; // CXL offset of ResizeMeta
    int mds_id_ = 0;
    int n_mds_ = 1;
    uint64_t notify_offsets_[kMaxMetaNumOfGroup] = {
        0}; // GIM notify offsets [n_mds_]

    ResizeConfig() = default;

    ResizeConfig(uint64_t meta_off, int mds, int n_mds_,
                 const uint64_t *offsets)
        : meta_offset_(meta_off), mds_id_(mds), n_mds_(n_mds_) {
      if (offsets != nullptr) {
        for (int i = 0; i < n_mds_; i++) {
          notify_offsets_[i] = offsets[i];
        }
      }
    }
  };

  TwoLevelHashtable(size_t tl_num_buckets, int n_mds_ = 1)
      : tl_n_(tl_num_buckets), bl_n_(tl_num_buckets / 2), n_mds_(n_mds_) {
    assert(tl_num_buckets >= 2);
    assert((tl_num_buckets & (tl_num_buckets - 1)) == 0);
    assert(gDevice != nullptr);
    tl_offset_ = gDevice->CXLMemMalloc(tl_n_ * kBucketSize);
    bl_offset_ = gDevice->CXLMemMalloc(bl_n_ * kBucketSize);
    init_hint();
  }

  TwoLevelHashtable(size_t tl_num_buckets, uint64_t tl_offset,
                    uint64_t bl_offset, int n_mds_ = 1)
      : tl_offset_(tl_offset), bl_offset_(bl_offset), tl_n_(tl_num_buckets),
        bl_n_(tl_num_buckets / 2), n_mds_(n_mds_) {
    init_hint();
  }

  ~TwoLevelHashtable() {
#ifdef LEVEL_HASHTABLE_TAG_HINT
    delete owned_hint_;
    delete prev_hint_;
#endif
  }

  void SetMds(int n_mds, const uint64_t *offsets) {
    n_mds_ = n_mds;
    rcfg_.n_mds_ = n_mds_;
    if (offsets != nullptr) {
      for (int i = 0; i < n_mds_; i++) {
        rcfg_.notify_offsets_[i] = offsets[i];
      }
    }
  }

  static size_t RequiredMemory(size_t tl_num_buckets) {
    return (tl_num_buckets + tl_num_buckets / 2) * kBucketSize;
  }

  // ── Enable progressive resize (call once after construction) ──
  void enable_resize(ResizeConfig cfg) {
    rcfg_ = cfg;
    resize_enabled_ = true;
    resize_cache_.version = read_meta_version();
  }

  int insert(const K &key, const V &val) {
    for (int attempt = 0; attempt < kEpochRetryMax; ++attempt) {
      pre_op();
      uint64_t e0 = layout_epoch_.load(std::memory_order_acquire);
      int r = insert_impl(key, val);
      if (r == 1) {
        post_op();
        return 1;
      }
      if (r == 0 && resize_enabled_ &&
          !migration_.active.load(std::memory_order_acquire)) {
        do_resize();
        r = insert_impl(key, val);
        if (r == 1) {
          post_op();
          return 1;
        }
      }
      uint64_t e1 = layout_epoch_.load(std::memory_order_acquire);
      post_op();
      if (e0 == e1)
        return r;
    }
    return 0;
  }

  bool find(const K &key, V &val) {
    for (int attempt = 0; attempt < kEpochRetryMax; ++attempt) {
      pre_op();
      uint64_t e0 = layout_epoch_.load(std::memory_order_acquire);
      bool r = find_impl(key, val);
      uint64_t e1 = layout_epoch_.load(std::memory_order_acquire);
      post_op();
      if (r || e0 == e1)
        return r;
    }
    return false;
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
    for (int attempt = 0; attempt < kEpochRetryMax; ++attempt) {
      pre_op();
      uint64_t e0 = layout_epoch_.load(std::memory_order_acquire);
      bool r = update_impl(key, [&](V &cur) { func(cur); });
      uint64_t e1 = layout_epoch_.load(std::memory_order_acquire);
      post_op();
      if (r || e0 == e1)
        return r;
    }
    return false;
  }

  bool erase(const K &key) {
    V d;
    return erase(key, d);
  }

  bool erase(const K &key, V &out_val) {
    for (int attempt = 0; attempt < kEpochRetryMax; ++attempt) {
      pre_op();
      uint64_t e0 = layout_epoch_.load(std::memory_order_acquire);
      bool r = erase_impl(key, out_val);
      uint64_t e1 = layout_epoch_.load(std::memory_order_acquire);
      post_op();
      if (r || e0 == e1)
        return r;
    }
    return false;
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

  uint64_t GetElementNum() const {
    return size_.load(std::memory_order_relaxed);
  }

  uint64_t GetSpaceNum() const { return (tl_n_ + bl_n_) * kSlots; }

  double load_factor() const {
    return static_cast<double>(GetElementNum()) / GetSpaceNum();
  }

  double estimated_load_factor() const {
    return static_cast<double>(size_.load(std::memory_order_relaxed) * n_mds_) /
           GetSpaceNum();
  }

  size_t GetSpace() const { return (tl_n_ + bl_n_) * kBucketSize; }

  size_t tl_num_buckets() const { return tl_n_; }

  size_t bl_num_buckets() const { return bl_n_; }
#ifdef LEVEL_HASHTABLE_TAG_HINT
  TagHint *hint() const { return hint_; }
#endif
  uint64_t tl_offset() const { return tl_offset_; }

  uint64_t bl_offset() const { return bl_offset_; }

private:
  struct MigrationState {
    std::atomic<bool> active{false};
    uint64_t old_bl_offset = 0;
    size_t old_bl_n = 0;
  };

  void begin_migration() {
    migration_.old_bl_offset = resize_cache_.old_bl_offset;
    migration_.old_bl_n = resize_cache_.old_bl_size;
    tl_offset_ = resize_cache_.tl_offset;
    tl_n_ = resize_cache_.tl_size;
    bl_offset_ = resize_cache_.bl_offset;
    bl_n_ = resize_cache_.bl_size;

#ifdef LEVEL_HASHTABLE_TAG_HINT
    auto *new_hint = new TagHint(tl_n_ + bl_n_);
    delete prev_hint_;
    prev_hint_ = owned_hint_;
    owned_hint_ = new_hint;
    hint_ = new_hint;
#endif

    migration_.active.store(true, std::memory_order_release);
    layout_epoch_.fetch_add(1, std::memory_order_release);
  }

  void end_migration() {
    migration_.active.store(false, std::memory_order_release);
    layout_epoch_.fetch_add(1, std::memory_order_release);
  }

  void adopt_layout() {
    bool exp = false;
    if (!adopting_.compare_exchange_strong(exp, true,
                                           std::memory_order_acq_rel)) {
      while (adopting_.load(std::memory_order_acquire))
        _mm_pause();
      return;
    }
    tl_offset_ = resize_cache_.tl_offset;
    tl_n_ = resize_cache_.tl_size;
    bl_offset_ = resize_cache_.bl_offset;
    bl_n_ = resize_cache_.bl_size;
#ifdef LEVEL_HASHTABLE_TAG_HINT
    auto *new_hint = new TagHint(tl_n_ + bl_n_);
    delete prev_hint_;
    prev_hint_ = owned_hint_;
    owned_hint_ = new_hint;
    hint_ = new_hint;
#endif
    std::atomic_thread_fence(std::memory_order_release);
    layout_epoch_.fetch_add(1, std::memory_order_release);
    adopting_.store(false, std::memory_order_release);
  }

  void wait_layout_ready() {
    while (migration_begun_.load(std::memory_order_acquire)) {
      if (migration_.active.load(std::memory_order_acquire)) {
        return;
      }
      _mm_pause();
    }
    std::atomic_thread_fence(std::memory_order_acquire);
  }

  void read_meta_full(ResizeMeta *out) const {
    gDevice->CXLReadSync(rcfg_.meta_offset_, 64, out);
  }

  uint64_t read_meta_ctrl() const {
    alignas(64) ResizeMeta m;
    read_meta_full(&m);
    return m.ctrl;
  }

  bool cas_meta_ctrl(uint64_t expected, uint64_t desired) {
    uint64_t old;
    return gDevice->CXLAtomicCasSync(rcfg_.meta_offset_, expected, desired,
                                     &old) == 0;
  }

  uint64_t read_meta_version() const {
    alignas(64) ResizeMeta m;
    read_meta_full(&m);
    return m.version;
  }

  void increment_done_count() {
    for (;;) {
      uint64_t ctrl = read_meta_ctrl();
      uint8_t dc = ResizeCtrl{ctrl}.done_count();
      uint64_t new_ctrl =
          (ctrl & ~(0xFFULL << 24)) | ((uint64_t)(dc + 1) << 24);
      if (cas_meta_ctrl(ctrl, new_ctrl))
        break;
    }
  }

  void notify_all_mds(uint64_t new_version) {
    alignas(64) char buf[64] = {};
    std::memcpy(buf, &new_version, 8);
    for (int i = 0; i < rcfg_.n_mds_; ++i) {
      if (i == rcfg_.mds_id_)
        continue;
      gDevice->GIMWriteSync(i, rcfg_.notify_offsets_[i], 64, buf);
    }
    auto *my_ver = reinterpret_cast<uint64_t *>(
        gDevice->gim->MyBuf() + rcfg_.notify_offsets_[rcfg_.mds_id_]);
    *my_ver = new_version;
  }

  uint64_t poll_local_version() const {
    auto *ver = reinterpret_cast<const uint64_t *>(
        gDevice->gim->MyBuf() + rcfg_.notify_offsets_[rcfg_.mds_id_]);
    return *ver;
  }

  bool check_version_changed() const {
    return poll_local_version() != resize_cache_.version;
  }

  void pre_op() {
    if (!resize_enabled_)
      return;
    check_resize();
  }

  void post_op() {
    if (!resize_enabled_ || !migration_begun_.load(std::memory_order_acquire))
      return;
    migrate_step();
    check_finalize();
  }

  void check_resize() {
    if (migration_begun_.load(std::memory_order_acquire)) {
      if (!migration_.active.load(std::memory_order_acquire)) {
        wait_layout_ready();
      }
      // Already migrating: check if finalize happened
      if (check_version_changed()) {
        ResizeCtrl ctrl{read_meta_ctrl()};
        if (ctrl.state() == (uint8_t)ResizeState::NORMAL) {
          bool exp = true;
          if (migration_begun_.compare_exchange_strong(
                  exp, false, std::memory_order_acq_rel)) {
            refresh_cache();
            end_migration();
          }
        }
      }
      return;
    }

    // Not migrating: check for new resize event (lightweight CXL read)
    if (!check_version_changed())
      return;

    // Detected version change — read current state
    ResizeCtrl ctrl{read_meta_ctrl()};
    if (ctrl.state() == (uint8_t)ResizeState::MIGRATING) {
      // Resize in progress
      bool exp = false;
      if (migration_begun_.compare_exchange_strong(exp, true,
                                                   std::memory_order_acq_rel)) {
        while (true) {
          refresh_cache();
          if (resize_cache_.tl_size > 0)
            break;
          _mm_pause();
        }
        begin_migration();
      } else {
        wait_layout_ready();
      }
    } else if (ctrl.state() == (uint8_t)ResizeState::NORMAL) {
      // Resize completed while we weren't looking: catch up to new layout
      refresh_cache();
      adopt_layout();
    }
  }

  // Called when insert is full: initiate resize + begin migration
  void do_resize() {
    if (check_version_changed()) {
      refresh_cache();
      if (resize_cache_.state == ResizeState::NORMAL) {
        adopt_layout();
        return;
      }
      join_migration();
      return;
    }
    try_initiate_resize();

    bool exp = false;
    if (migration_begun_.compare_exchange_strong(exp, true,
                                                 std::memory_order_acq_rel)) {
      refresh_cache();
      if (resize_cache_.state == ResizeState::MIGRATING) {
        while (resize_cache_.tl_size == 0) {
          _mm_pause();
          refresh_cache();
        }
        begin_migration();
      } else {
        migration_begun_.store(false, std::memory_order_release);
        adopt_layout();
      }
    } else {
      wait_layout_ready();
    }
  }

  // Join an ongoing migration initiated by another MDS.
  // Waits for full ResizeMeta to be ready, then switches to new layout.
  void join_migration() {
    while (resize_cache_.tl_size == 0) {
      _mm_pause();
      refresh_cache();
    }
    bool exp = false;
    if (migration_begun_.compare_exchange_strong(exp, true,
                                                 std::memory_order_acq_rel)) {
      begin_migration();
    } else {
      wait_layout_ready();
    }
  }

  // CAS winner allocates new_TL; loser waits for winner's version bump.
  bool try_initiate_resize() {
    uint64_t ctrl = read_meta_ctrl();
    if (ResizeCtrl{ctrl}.state() != (uint8_t)ResizeState::NORMAL) {
      uint64_t cur_ver = resize_cache_.version;
      while (read_meta_version() == cur_ver)
        _mm_pause();
      return false;
    }

    uint64_t new_ctrl = (ctrl & ~0xFFULL) | (uint64_t)ResizeState::MIGRATING;
    if (!cas_meta_ctrl(ctrl, new_ctrl)) {
      uint64_t cur_ver = resize_cache_.version;
      while (read_meta_version() == cur_ver)
        _mm_pause();
      return false;
    }

    SPDLOG_INFO("resize: load={:.2f}, tl_buckets={}, bl_buckets={}",
                estimated_load_factor(), tl_num_buckets(), bl_num_buckets());

    alignas(64) ResizeMeta prev{};
    read_meta_full(&prev);
    if (prev.tl_size > 0) {
      tl_offset_ = prev.tl_offset;
      tl_n_ = prev.tl_size;
      bl_offset_ = prev.bl_offset;
      bl_n_ = prev.bl_size;
    }

    uint64_t new_version = prev.version + 1;
    {
      alignas(64) ResizeMeta early{};
      early.ctrl =
          ResizeCtrl::pack((uint8_t)ResizeState::MIGRATING,
                           (uint8_t)rcfg_.mds_id_, (uint8_t)rcfg_.n_mds_, 0);
      early.version = new_version;
      early.tl_offset = 0;
      early.tl_size = 0;
      _mm_sfence();
      gDevice->CXLWriteSync(rcfg_.meta_offset_, 64, &early);

      notify_all_mds(new_version);
    }

    size_t new_tl_n = tl_n_ * 2;
    uint64_t new_tl_off = gDevice->CXLMemMalloc(new_tl_n * kBucketSize);

    alignas(64) ResizeMeta m{};
    m.ctrl = ResizeCtrl::pack((uint8_t)ResizeState::MIGRATING,
                              (uint8_t)rcfg_.mds_id_, (uint8_t)rcfg_.n_mds_, 0);
    m.version = new_version;
    m.tl_offset = new_tl_off;
    m.tl_size = new_tl_n;
    m.bl_offset = tl_offset_;
    m.bl_size = tl_n_;
    m.old_bl_offset = bl_offset_;
    m.old_bl_size = bl_n_;
    _mm_sfence();
    gDevice->CXLWriteSync(rcfg_.meta_offset_, 64, &m);

    notify_all_mds(new_version);

    refresh_cache();
    return true;
  }

  // Migrates one bucket from old_BL.
  void migrate_step() {
    if (resize_cache_.state != ResizeState::MIGRATING ||
        resize_cache_.range_done.load(std::memory_order_acquire))
      return;

    size_t i = resize_cache_.local_cursor.load(std::memory_order_relaxed);
    if (i >= resize_cache_.range_end) {
      if (!resize_cache_.done_reported.exchange(true,
                                                std::memory_order_acq_rel)) {
        increment_done_count();
      }
      resize_cache_.range_done.store(true, std::memory_order_release);
      return;
    }

    uint64_t old_bl = resize_cache_.old_bl_offset;
    uint64_t hr_off = old_bl + i * kBucketSize;

    alignas(64) uint64_t metas[kSlots];
    gDevice->CXLReadSync(hr_off, kHashRegionSize, metas);

    if (metas[0] & kDirtyBit)
      return;

    bool has_entries = false;
    for (int j = 0; j < kSlots; ++j) {
      if ((metas[j] & kFpMask) != 0) {
        has_entries = true;
        break;
      }
    }
    if (!has_entries) {
      resize_cache_.local_cursor.compare_exchange_strong(
          i, i + 1, std::memory_order_relaxed);
      return;
    }

    uint64_t old;
    if (gDevice->CXLAtomicCasSync(hr_off, metas[0], make_locked(metas[0]),
                                  &old) != 0)
      return;

    bool migrated[kSlots] = {};
    int occupied = 0;
    int succeeded = 0;
    for (int j = 0; j < kSlots; ++j) {
      uint64_t sfp = metas[j] & kFpMask;
      if (sfp == 0) {
        migrated[j] = true; // empty slot — nothing to carry over
        continue;
      }
      ++occupied;
      uint64_t v_off =
          old_bl + i * kBucketSize + kHashRegionSize + j * kValueSize;
      alignas(64) char vbuf[kValueSize];
      gDevice->CXLReadSync(v_off, kValueSize, vbuf);
      V val;
      std::memcpy(&val, vbuf, sizeof(V));
      if (insert_to_new_layout(sfp, val)) {
        migrated[j] = true;
        ++succeeded;
      }
    }

    // Write back old bucket:
    //   - migrated slots  → cleared (fp = 0)
    //   - failed slots    → preserve original fp for the next retry
    //   - meta[0]         → version bumped and D-bit cleared via
    //   set_unlocked_meta0
    alignas(64) uint64_t new_metas[kSlots] = {};
    for (int j = 1; j < kSlots; ++j) {
      new_metas[j] = migrated[j] ? 0 : (metas[j] & kFpMask);
    }
    uint64_t meta0_fp = migrated[0] ? 0 : (metas[0] & kFpMask);
    set_unlocked_meta0(new_metas, metas[0], meta0_fp);
    gDevice->CXLWriteSync(hr_off, kHashRegionSize, new_metas);

    if (succeeded == occupied) {
      resize_cache_.local_cursor.compare_exchange_strong(
          i, i + 1, std::memory_order_relaxed);
    } else {
      // Partial migration: leave cursor in place so we retry this bucket
      // on the next migrate_step. Contention typically clears within a
      // handful of iterations; a sustained stall here means the new
      // layout is genuinely full and a follow-up resize is required,
      // which the next insert hitting the fullness check will trigger.
      SPDLOG_WARN("migrate_step: partial migration at bucket {} "
                  "({}/{} slots moved, rest preserved for retry)",
                  i, succeeded, occupied);
    }
  }

  // Insert entry to new layout during migration (2-pass: skip dirty → retry).
  // Returns true on success, false if no slot could be claimed after both
  // passes. On false the caller MUST preserve this entry in old_BL so a
  // later migrate_step call can retry — silently dropping it would lose
  // data.
  //
  // Failure causes, in rough order of likelihood:
  //   • Transient contention: another MDS is holding the target bucket's
  //     D-bit. Clears on the next migrate_step once that MDS finishes.
  //   • Genuine fullness: new_TL and new_BL candidates are all saturated.
  //     Extremely unlikely immediately after a 2× resize, but possible
  //     under very high load. A subsequent insert on the hot path will
  //     notice the fullness and trigger another resize.
  bool insert_to_new_layout(uint64_t fp, const V &val) {
    K key = key_traits_.extract(val);
    auto cands = get_candidates(key);
    bool contended[6] = {};

    // Pass 1: try non-dirty, non-full buckets
    for (int c = 0; c < cands.count; ++c) {
      if (cands.level[c] == 2)
        continue;
      uint64_t base = cands.base[c];
      size_t bi = cands.idx[c];
      uint64_t hr_off = base + bi * kBucketSize;

      alignas(64) uint64_t keys[kSlots];
      gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);

      if (keys[0] & kDirtyBit) {
        contended[c] = true;
        continue;
      }
      if (!has_empty_slot(keys))
        continue;

      uint64_t old;
      if (gDevice->CXLAtomicCasSync(hr_off, keys[0], make_locked(keys[0]),
                                    &old) != 0) {
        contended[c] = true;
        continue;
      }

      int target = find_empty_slot(keys);
      if (target < 0) {
        unlock_bucket(hr_off, keys);
        continue;
      }

      uint64_t v_off =
          base + bi * kBucketSize + kHashRegionSize + target * kValueSize;
      alignas(64) char vbuf[kValueSize] = {};
      std::memcpy(vbuf, &val, sizeof(V));
      gDevice->CXLWriteSync(v_off, kValueSize, vbuf);
      commit_bucket(hr_off, keys, target, fp);
      HT_STAT_INC(hint_set_migrate);
      hint_set(global_bucket_id(cands.level[c], bi), target, fp);
      return true;
    }

    // Pass 2: wait for contended buckets and retry
    for (int c = 0; c < cands.count; ++c) {
      if (!contended[c] || cands.level[c] == 2)
        continue;
      uint64_t base = cands.base[c];
      size_t bi = cands.idx[c];
      uint64_t hr_off = base + bi * kBucketSize;

      for (int retry = 0; retry < 3; ++retry) {
        alignas(64) uint64_t keys[kSlots];
        gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
        if (keys[0] & kDirtyBit) {
          if (!wait_for_unlock(hr_off, keys))
            break;
        }
        if (!has_empty_slot(keys))
          break;

        uint64_t old;
        if (gDevice->CXLAtomicCasSync(hr_off, keys[0], make_locked(keys[0]),
                                      &old) != 0)
          continue;

        int target = find_empty_slot(keys);
        if (target < 0) {
          unlock_bucket(hr_off, keys);
          break;
        }

        uint64_t v_off =
            base + bi * kBucketSize + kHashRegionSize + target * kValueSize;
        alignas(64) char vbuf[kValueSize] = {};
        std::memcpy(vbuf, &val, sizeof(V));
        gDevice->CXLWriteSync(v_off, kValueSize, vbuf);
        commit_bucket(hr_off, keys, target, fp);
        HT_STAT_INC(hint_set_migrate);
        hint_set(global_bucket_id(cands.level[c], bi), target, fp);
        return true;
      }
    }

    if (try_migrate_and_insert(key, val, fp, cands) == 1) {
      size_.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  // Initiator: check if all MDS done, transition MIGRATING → NORMAL.
  void check_finalize() {
    if (!migration_begun_.load(std::memory_order_acquire) ||
        !resize_cache_.range_done.load(std::memory_order_acquire))
      return;

    uint64_t ctrl = read_meta_ctrl();
    ResizeCtrl rc{ctrl};
    if (rcfg_.mds_id_ != rc.initiator())
      return;
    if (rc.done_count() < rc.n_mds_())
      return;

    uint64_t new_ctrl = (ctrl & ~0xFFULL) | (uint64_t)ResizeState::NORMAL;
    if (!cas_meta_ctrl(ctrl, new_ctrl))
      return;

    alignas(64) ResizeMeta m;
    read_meta_full(&m);
    m.ctrl = new_ctrl;
    m.version = m.version + 1;
    gDevice->CXLWriteSync(rcfg_.meta_offset_, 64, &m);

    notify_all_mds(m.version);

    refresh_cache();
    end_migration();
    migration_begun_.store(false, std::memory_order_release);
  }

  void refresh_cache() {
    alignas(64) ResizeMeta m;
    read_meta_full(&m);

    ResizeCtrl ctrl{m.ctrl};
    resize_cache_.state = static_cast<ResizeState>(ctrl.state());
    resize_cache_.version = m.version;
    resize_cache_.tl_offset = m.tl_offset;
    resize_cache_.tl_size = m.tl_size;
    resize_cache_.bl_offset = m.bl_offset;
    resize_cache_.bl_size = m.bl_size;
    resize_cache_.old_bl_offset = m.old_bl_offset;
    resize_cache_.old_bl_size = m.old_bl_size;

    if (resize_cache_.state == ResizeState::MIGRATING &&
        resize_cache_.old_bl_size > 0 && ctrl.n_mds_() > 0) {
      size_t chunk = resize_cache_.old_bl_size / ctrl.n_mds_();
      resize_cache_.range_end = (rcfg_.mds_id_ == ctrl.n_mds_() - 1)
                                    ? resize_cache_.old_bl_size
                                    : (rcfg_.mds_id_ + 1) * chunk;
      resize_cache_.local_cursor.store(rcfg_.mds_id_ * chunk,
                                       std::memory_order_relaxed);
      resize_cache_.range_done.store(false, std::memory_order_relaxed);
      resize_cache_.done_reported.store(false, std::memory_order_relaxed);
    } else {
      resize_cache_.range_done.store(true, std::memory_order_relaxed);
    }
  }

  int insert_impl(const K &key, const V &val) {
    HT_STAT_INC(insert_calls);
    uint64_t fp = make_fp(key);
    auto cands = get_candidates(key);
    HT_STAT_ADD(insert_cand_probes, cands.count);
    bool contended[6] = {};

    for (int c = 0; c < cands.count; ++c) {
      if (cands.level[c] == 2)
        continue;
      uint64_t base = cands.base[c];
      size_t bi = cands.idx[c];
      uint64_t hr_off = base + bi * kBucketSize;

      alignas(64) uint64_t keys[kSlots];
      HT_STAT_INC(insert_pass1_reads);
      HT_STAT_INC(insert_cxl_hr_reads);
      gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);

      if (keys[0] & kDirtyBit) {
        contended[c] = true;
        continue;
      }
      if (!has_empty_slot(keys))
        continue;

      int r = try_insert_into(base, bi, hr_off, keys, fp, val, cands.level[c]);
      if (r == 1) {
        HT_STAT_INC(insert_pass1_placed);
        HT_STAT_INC(insert_hits);
        return 1;
      }
      if (r == -1)
        contended[c] = true;
    }

    for (int c = 0; c < cands.count; ++c) {
      if (!contended[c] || cands.level[c] == 2)
        continue;
      uint64_t base = cands.base[c];
      size_t bi = cands.idx[c];
      uint64_t hr_off = base + bi * kBucketSize;

      for (int retry = 0; retry < 3; ++retry) {
        alignas(64) uint64_t keys[kSlots];
        HT_STAT_INC(insert_pass2_reads);
        HT_STAT_INC(insert_cxl_hr_reads);
        gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
        if (keys[0] & kDirtyBit) {
          if (!wait_for_unlock(hr_off, keys))
            break;
        }
        if (!has_empty_slot(keys))
          break;
        int r =
            try_insert_into(base, bi, hr_off, keys, fp, val, cands.level[c]);
        if (r == 1) {
          HT_STAT_INC(insert_pass2_placed);
          HT_STAT_INC(insert_hits);
          return 1;
        }
        if (r == 0)
          break;
      }
    }

    HT_STAT_INC(insert_cuckoo_calls);
    int r = try_migrate_and_insert(key, val, fp, cands);
    if (r == 1) {
      HT_STAT_INC(insert_cuckoo_placed);
      HT_STAT_INC(insert_hits);
    } else {
      HT_STAT_INC(insert_misses);
    }
    return r;
  }

  static uint64_t make_locked(uint64_t meta0) {
    uint64_t fp = meta0 & kFpMask;
    uint64_t ver = ((meta0 >> kVersionShift) & 0x7) + 1;
    return kDirtyBit | ((ver & 0x7) << kVersionShift) | fp;
  }

  static uint64_t make_unlocked(uint64_t pre_cas_meta0, uint64_t fp) {
    uint64_t ver = ((pre_cas_meta0 >> kVersionShift) & 0x7) + 1;
    return ((ver & 0x7) << kVersionShift) | fp;
  }

  static void set_unlocked_meta0(uint64_t *new_keys, uint64_t pre_cas_meta0,
                                 uint64_t fp) {
    new_keys[0] = make_unlocked(pre_cas_meta0, fp);
  }

  int try_insert_into(uint64_t base, size_t bi, uint64_t hr_off, uint64_t *keys,
                      uint64_t fp, const V &val, int level) {
    uint64_t old;
    if (gDevice->CXLAtomicCasSync(hr_off, keys[0], make_locked(keys[0]),
                                  &old) != 0)
      return -1;
    int target = find_empty_slot(keys);
    if (target < 0) {
      HT_STAT_INC(insert_cxl_hr_writes); // unlock_bucket
      unlock_bucket(hr_off, keys);
      return 0;
    }
    uint64_t v_off =
        base + bi * kBucketSize + kHashRegionSize + target * kValueSize;
    alignas(64) char vbuf[kValueSize] = {};
    std::memcpy(vbuf, &val, sizeof(V));
    HT_STAT_INC(insert_cxl_val_writes);
    gDevice->CXLWriteSync(v_off, kValueSize, vbuf);
    HT_STAT_INC(insert_cxl_hr_writes); // commit_bucket
    commit_bucket(hr_off, keys, target, fp);
    size_.fetch_add(1, std::memory_order_relaxed);
    HT_STAT_INC(hint_set_insert);
    hint_set(global_bucket_id(level, bi), target, fp);
    return 1;
  }

  bool erase_impl(const K &key, V &out_val) {
    HT_STAT_INC(erase_calls);
    uint64_t fp = make_fp(key);
    auto cands = get_candidates(key);
    HT_STAT_ADD(erase_cand_probes, cands.count);
    bool contended[6] = {};
    bool old_bl_contended = false;
    bool searched[6] = {};

    auto try_bucket = [&](int c) -> int {
      // Returns: 1 = erased, 0 = no match, -1 = contended (caller handles).
      uint64_t base = cands.base[c];
      size_t bi = cands.idx[c];
      uint64_t hr_off = base + bi * kBucketSize;
      alignas(64) uint64_t keys[kSlots];
      HT_STAT_INC(erase_cxl_hr_reads);
      gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
      if (keys[0] & kDirtyBit) {
        contended[c] = true;
        if (cands.level[c] == 2)
          old_bl_contended = true;
        return -1;
      }
      if (find_fp_slot(keys, fp) < 0)
        return 0;
      int r = try_erase_from(base, bi, hr_off, keys, fp, key, out_val,
                             cands.level[c]);
      if (r == 1)
        return 1;
      if (r == -1) {
        contended[c] = true;
        if (cands.level[c] == 2)
          old_bl_contended = true;
        return -1;
      }
      return 0;
    };

#ifdef LEVEL_HASHTABLE_TAG_HINT
    // Phase 1: hint-guided. Only probe candidates whose hint row reports a
    // potential match — skips the HR read for all hint-negative candidates,
    // which is the main win. A stale-negative hint just defers the erase to
    // phase 2 below (no correctness issue; erase must not be lost, so the
    // fallback is mandatory).
    TagHint *h = hint_;
    if (h) {
      for (int c = 0; c < cands.count; ++c) {
        if (!h->test(global_bucket_id(cands.level[c], cands.idx[c]), fp)) {
          HT_STAT_INC(erase_phase1_hint_neg);
          continue;
        }
        HT_STAT_INC(erase_phase1_hint_pos);
        searched[c] = true;
        int r = try_bucket(c);
        if (r == 1) {
          HT_STAT_INC(erase_phase1_found);
          HT_STAT_INC(erase_hits);
          return true;
        }
        if (r == 0)
          HT_STAT_INC(erase_phase1_false_pos);
        // r == -1: contended, deferred to phase 3.
      }
    }
#endif

    // Phase 2: fallback for hint-negative candidates (and for every candidate
    // when TAG_HINT is off). On success, try_erase_from clears the tag.
    for (int c = 0; c < cands.count; ++c) {
      if (searched[c])
        continue;
      HT_STAT_INC(erase_phase2_reads);
      int r = try_bucket(c);
      if (r == 1) {
        HT_STAT_INC(erase_phase2_found);
        HT_STAT_INC(erase_hits);
        return true;
      }
    }

    for (int c = 0; c < cands.count; ++c) {
      if (!contended[c])
        continue;
      uint64_t base = cands.base[c];
      size_t bi = cands.idx[c];
      uint64_t hr_off = base + bi * kBucketSize;
      for (;;) {
        alignas(64) uint64_t keys[kSlots];
        HT_STAT_INC(erase_phase3_retries);
        HT_STAT_INC(erase_cxl_hr_reads);
        gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
        if (keys[0] & kDirtyBit) {
          if (!wait_for_unlock(hr_off, keys))
            break;
        }
        if (find_fp_slot(keys, fp) < 0)
          break;
        int r = try_erase_from(base, bi, hr_off, keys, fp, key, out_val,
                               cands.level[c]);
        if (r == 1) {
          HT_STAT_INC(erase_hits);
          return true;
        }
        if (r == 0)
          break;
        // CAS contention: retry (erase must not be lost)
        _mm_pause();
      }
    }

    if (old_bl_contended) {
      for (int c = 0; c < cands.count; ++c) {
        if (cands.level[c] == 2)
          continue;
        uint64_t base = level_offset(cands.level[c]);
        size_t bi = cands.idx[c];
        uint64_t hr_off = base + bi * kBucketSize;
        alignas(64) uint64_t keys[kSlots];
        HT_STAT_INC(erase_cxl_hr_reads);
        gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
        if ((keys[0] & kDirtyBit) || find_fp_slot(keys, fp) < 0)
          continue;
        int r = try_erase_from(base, bi, hr_off, keys, fp, key, out_val,
                               cands.level[c]);
        if (r == 1) {
          HT_STAT_INC(erase_hits);
          return true;
        }
      }
    }
    HT_STAT_INC(erase_misses);
    return false;
  }

  int try_erase_from(uint64_t base, size_t bi, uint64_t hr_off, uint64_t *keys,
                     uint64_t fp, const K &key, V &out_val, int level) {
    uint64_t old;
    if (gDevice->CXLAtomicCasSync(hr_off, keys[0], make_locked(keys[0]),
                                  &old) != 0)
      return -1;
    int del_slot = -1;
    for (int s = 0; s < kSlots; ++s) {
      if (slot_fp(keys, s) != fp)
        continue;
      uint64_t v_off =
          base + bi * kBucketSize + kHashRegionSize + s * kValueSize;
      alignas(64) char vbuf[kValueSize];
      HT_STAT_INC(erase_cxl_val_reads);
      gDevice->CXLReadSync(v_off, kValueSize, vbuf);
      V candidate;
      std::memcpy(&candidate, vbuf, sizeof(V));
      if (key_traits_.match(candidate, key)) {
        del_slot = s;
        out_val = candidate;
        break;
      }
    }
    if (del_slot < 0) {
      HT_STAT_INC(erase_cxl_hr_writes);
      unlock_bucket(hr_off, keys);
      return 0;
    }
    alignas(64) uint64_t new_keys[kSlots];
    std::memcpy(new_keys, keys, kHashRegionSize);
    new_keys[del_slot] = 0;
    set_unlocked_meta0(new_keys, keys[0],
                       del_slot == 0 ? 0 : (keys[0] & kFpMask));
    HT_STAT_INC(erase_cxl_hr_writes);
    gDevice->CXLWriteSync(hr_off, kHashRegionSize, new_keys);
    size_.fetch_sub(1, std::memory_order_relaxed);
    if (level != 2) {
      HT_STAT_INC(hint_clear_erase);
      hint_clear(global_bucket_id(level, bi), del_slot);
    }
    return 1;
  }

  static uint64_t slot_fp(const uint64_t *keys, int s) {
    return keys[s] & kFpMask;
  }

  static bool has_empty_slot(const uint64_t *keys) {
    for (int s = 0; s < kSlots; ++s)
      if ((keys[s] & kFpMask) == 0)
        return true;
    return false;
  }

  static int find_empty_slot(const uint64_t *keys) {
    for (int s = 0; s < kSlots; ++s)
      if ((keys[s] & kFpMask) == 0)
        return s;
    return -1;
  }

  static int find_fp_slot(const uint64_t *keys, uint64_t fp) {
    for (int s = 0; s < kSlots; ++s) {
      uint64_t sfp = keys[s] & kFpMask;
      if (sfp != 0 && sfp == fp)
        return s;
    }
    return -1;
  }

  static void unlock_bucket(uint64_t hr_off, const uint64_t *keys) {
    alignas(64) uint64_t new_keys[kSlots];
    std::memcpy(new_keys, keys, kHashRegionSize);
    set_unlocked_meta0(new_keys, keys[0], keys[0] & kFpMask);
    gDevice->CXLWriteSync(hr_off, kHashRegionSize, new_keys);
  }

  static void commit_bucket(uint64_t hr_off, const uint64_t *keys, int target,
                            uint64_t new_fp) {
    alignas(64) uint64_t new_keys[kSlots];
    std::memcpy(new_keys, keys, kHashRegionSize);
    new_keys[target] = new_fp;
    uint64_t meta0_fp = (target == 0) ? new_fp : (keys[0] & kFpMask);
    set_unlocked_meta0(new_keys, keys[0], meta0_fp);
    gDevice->CXLWriteSync(hr_off, kHashRegionSize, new_keys);
  }

  static bool wait_for_unlock(uint64_t hr_off, uint64_t *keys,
                              int max_spins = 1000) {
    for (int i = 0; i < max_spins; ++i) {
      gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
      if (!(keys[0] & kDirtyBit))
        return true;
    }
    uint64_t cleared = keys[0] & ~kDirtyBit;
    uint64_t old;
    if (gDevice->CXLAtomicCasSync(hr_off, keys[0], cleared, &old) == 0)
      SPDLOG_WARN("Bucket D-bit timeout: force cleared hr={:#x}", hr_off);
    gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
    return !(keys[0] & kDirtyBit);
  }

  uint64_t level_offset(int level) const {
    if (level == 0)
      return tl_offset_;
    if (level == 1)
      return bl_offset_;
    return migration_.old_bl_offset;
  }

  size_t global_bucket_id(int level, size_t bi) const {
    return level == 0 ? bi : tl_n_ + bi;
  }

  void init_hint() {
#ifdef LEVEL_HASHTABLE_TAG_HINT
    owned_hint_ = new TagHint(tl_n_ + bl_n_);
    hint_ = owned_hint_;
#endif
  }

#ifdef LEVEL_HASHTABLE_TAG_HINT
  void hint_set(size_t gbi, int slot_idx, uint64_t fp) const {
    TagHint *h = hint_;
    if (h) {
      h->set(gbi, slot_idx, fp);
    }
  }

  void hint_clear(size_t gbi, int slot_idx) const {
    TagHint *h = hint_;
    if (h) {
      h->clear(gbi, slot_idx);
    }
  }
#else
  void hint_set(size_t, int, uint64_t) const {}

  void hint_clear(size_t, int) const {}
#endif

  // ── Hash ──
  uint64_t make_fp(const K &key) const {
    uint64_t fp = hasher_(key) & kFpMask;
    return fp == 0 ? 1 : fp;
  }

  // Two independent SplitMix64-style mixers, mirroring the original Zuo
  // 2018 Level-Hashing's two-seed F_HASH / S_HASH scheme. The F-candidate
  // uses mix_h1, the S-candidate uses mix_h2; the two streams are
  // statistically uncorrelated, which is what lets 1-hop cuckoo reach
  // ~0.9 load. The previous single-source design (h2 = h1 * golden_ratio)
  // capped real workloads at ~0.6 because all four candidates collapsed
  // onto h1's bit pattern.
  static uint64_t mix_h1(uint64_t h) {
    uint64_t z = h + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  static uint64_t mix_h2(uint64_t h) {
    uint64_t z = h + 0xC2B2AE3D27D4EB4FULL;
    z = (z ^ (z >> 33)) * 0xFF51AFD7ED558CCDULL;
    z = (z ^ (z >> 33)) * 0xC4CEB9FE1A85EC53ULL;
    return z ^ (z >> 33);
  }

  // F/S partitioning. Each level (TL / BL / old_BL) is split into a F-half
  // [0, n/2) and an S-half [n/2, n). The F-candidate of a key lives in F,
  // the S-candidate lives in S — guaranteeing the two candidates always
  // sit in different regions and the BL/TL index spaces don't share
  // low-bit structure.
  static size_t f_idx(uint64_t h, size_t level_n) { return h % (level_n / 2); }

  static size_t s_idx(uint64_t h, size_t level_n) {
    return h % (level_n / 2) + level_n / 2;
  }

  struct Candidates {
    size_t idx[6];
    int level[6];
    uint64_t base[6]; // bucket-region base, snapshotted with idx
    // Snapshotted level sizes — must match what `idx` was computed against.
    // Used by cuckoo movement (which needs the victim's alternate bucket on
    // the same level) and by global_bucket_id() for hint addressing.
    size_t tl_n;
    size_t bl_n;
    int count;
  };

  // Snapshot the entire layout (offsets + sizes + migration state) atomically.
  //
  // Without the active double-read, a writer's begin_migration() can interleave
  // with our separate atomic loads of tl_offset_/tl_n_/bl_offset_/bl_n_/
  // migration_.active and we get a half-old/half-new view: e.g. an OLD idx
  // (h1 % old_tl_n) used against a NEW tl_offset addresses a bucket that is
  // not a candidate of the key under either layout. Entries inserted under
  // such a view become unreachable to subsequent finds, which is what
  // produces the silent inode-missing failures we saw under high concurrency.
  //
  // begin_migration() writes layout vars before storing active=true (release);
  // end_migration() is symmetric. Reading active twice with the layout reads
  // sandwiched between guarantees that a flip is detected and we retry.
  Candidates get_candidates(const K &key) const {
    uint64_t raw = hasher_(key);
    uint64_t h1 = mix_h1(raw);
    uint64_t h2 = mix_h2(raw);

    bool active;
    uint64_t tl_off, bl_off, old_bl_off;
    size_t tl_n, bl_n, old_bl_n;
    for (;;) {
      bool a1 = migration_.active.load(std::memory_order_acquire);
      tl_off = tl_offset_.load(std::memory_order_acquire);
      tl_n = tl_n_.load(std::memory_order_acquire);
      bl_off = bl_offset_.load(std::memory_order_acquire);
      bl_n = bl_n_.load(std::memory_order_acquire);
      old_bl_off = migration_.old_bl_offset;
      old_bl_n = migration_.old_bl_n;
      std::atomic_thread_fence(std::memory_order_acquire);
      bool a2 = migration_.active.load(std::memory_order_acquire);
      if (a1 == a2) {
        active = a1;
        break;
      }
    }

    Candidates c{};
    c.tl_n = tl_n;
    c.bl_n = bl_n;
    if (!active) {
      c.idx[0] = f_idx(h1, tl_n);
      c.idx[1] = s_idx(h2, tl_n);
      c.idx[2] = f_idx(h1, bl_n);
      c.idx[3] = s_idx(h2, bl_n);
      c.level[0] = 0;
      c.level[1] = 0;
      c.level[2] = 1;
      c.level[3] = 1;
      c.base[0] = tl_off;
      c.base[1] = tl_off;
      c.base[2] = bl_off;
      c.base[3] = bl_off;
      c.count = 4;
    } else {
      c.idx[0] = f_idx(h1, old_bl_n);
      c.idx[1] = s_idx(h2, old_bl_n);
      c.idx[2] = f_idx(h1, tl_n);
      c.idx[3] = s_idx(h2, tl_n);
      c.idx[4] = f_idx(h1, bl_n);
      c.idx[5] = s_idx(h2, bl_n);
      c.level[0] = 2;
      c.level[1] = 2;
      c.level[2] = 0;
      c.level[3] = 0;
      c.level[4] = 1;
      c.level[5] = 1;
      c.base[0] = old_bl_off;
      c.base[1] = old_bl_off;
      c.base[2] = tl_off;
      c.base[3] = tl_off;
      c.base[4] = bl_off;
      c.base[5] = bl_off;
      c.count = 6;
    }
    return c;
  }

  // ── Find impl ──
  bool find_impl(const K &key, V &val) const {
    HT_STAT_INC(find_calls);
    uint64_t fp = make_fp(key);
    auto cands = get_candidates(key);
    HT_STAT_ADD(find_cand_probes, cands.count);
    bool contended[6] = {};
    bool old_bl_contended = false;

    bool searched[6] = {};
#ifdef LEVEL_HASHTABLE_TAG_HINT
    TagHint *h = hint_;
    if (h) {
      for (int c = 0; c < cands.count; ++c) {
        if (!h->test(global_bucket_id(cands.level[c], cands.idx[c]), fp)) {
          HT_STAT_INC(find_phase1_hint_neg);
          continue;
        }
        HT_STAT_INC(find_phase1_hint_pos);
        searched[c] = true;
        int r = find_in_bucket(cands.level[c], cands.idx[c], cands.base[c], fp,
                               key, val, false);
        if (r == 1) {
          HT_STAT_INC(find_phase1_found);
          HT_STAT_INC(find_hits);
          return true;
        }
        if (r == 0) {
          HT_STAT_INC(find_phase1_false_pos);
        }
        if (r == -1) {
          contended[c] = true;
          if (cands.level[c] == 2)
            old_bl_contended = true;
        }
      }
    }
#endif
    for (int c = 0; c < cands.count; ++c) {
      if (searched[c])
        continue;
      HT_STAT_INC(find_phase2_reads);
      int r = find_in_bucket(cands.level[c], cands.idx[c], cands.base[c], fp,
                             key, val);
      if (r == 1) {
        HT_STAT_INC(find_phase2_found);
        HT_STAT_INC(find_hits);
        return true;
      }
      if (r == -1) {
        contended[c] = true;
        if (cands.level[c] == 2)
          old_bl_contended = true;
      }
    }

    for (int c = 0; c < cands.count; ++c) {
      if (!contended[c])
        continue;
      uint64_t base = cands.base[c];
      uint64_t hr_off = base + cands.idx[c] * kBucketSize;
      for (;;) {
        HT_STAT_INC(find_phase3_retries);
        HT_STAT_INC(find_cxl_hr_reads);
        alignas(64) uint64_t keys[kSlots];
        gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
        if (keys[0] & kDirtyBit) {
          wait_for_unlock(hr_off, keys);
          _mm_pause();
          continue;
        }
        int r = scan_bucket_slots(cands.level[c], base, cands.idx[c], hr_off,
                                  keys, fp, key, val);
        if (r == 1) {
          HT_STAT_INC(find_hits);
          return true;
        }
        if (r == 0)
          break;
        _mm_pause();
      }
    }

    if (old_bl_contended) {
      for (int c = 0; c < cands.count; ++c) {
        if (cands.level[c] == 2)
          continue;
        int r = find_in_bucket(cands.level[c], cands.idx[c], cands.base[c], fp,
                               key, val);
        if (r == 1) {
          HT_STAT_INC(find_hits);
          return true;
        }
      }
    }
    HT_STAT_INC(find_misses);
    return false;
  }

  // `base` is the bucket-region offset for `level`, snapshotted in the caller's
  // Candidates so we don't re-resolve it via level_offset() and thereby reopen
  // the get_candidates ↔ level_offset split-snapshot race.
  int find_in_bucket(int level, size_t bi, uint64_t base, uint64_t fp,
                     const K &key, V &val, bool set_hint = true) const {
    uint64_t hr_off = base + bi * kBucketSize;
    alignas(64) uint64_t keys[kSlots];
    HT_STAT_INC(find_cxl_hr_reads);
    gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
    if (keys[0] & kDirtyBit)
      return -1;
    return scan_bucket_slots(level, base, bi, hr_off, keys, fp, key, val,
                             set_hint);
  }

  // Scans the 8 slots in a bucket for ``fp``. On a real match, lazily
  // repopulates the TagHint entry for (level, bi, matched_slot) with this
  // key's fp — this turns successful phase-2/phase-3 reads into warm-up
  // events for phase 1, so entries that existed before the last resize (and
  // therefore missed the hint wipe in begin_migration) get their tags
  // restored on first lookup instead of staying hint-dark forever.
  //
  // Correctness: we just validated both (1) the slot physically holds fp,
  // and (2) the meta word didn't flip between the key read and the value
  // read. Setting the tag overwrites any stale value in that slot, so this
  // never introduces false negatives. A racing erase after our set() can
  // leave a stale tag and later produce one extra phase-1 false-positive
  // read, which is strictly better than a persistent false negative.
  int scan_bucket_slots(int level, uint64_t base, size_t bi, uint64_t hr_off,
                        uint64_t *keys, uint64_t fp, const K &key, V &val,
                        bool set_hint = true) const {
    for (int s = 0; s < kSlots; ++s) {
      uint64_t sfp = slot_fp(keys, s);
      if (sfp == 0 || sfp != fp)
        continue;
      uint64_t v_off =
          base + bi * kBucketSize + kHashRegionSize + s * kValueSize;
      alignas(64) char vbuf[kValueSize];
      HT_STAT_INC(find_cxl_val_reads);
      gDevice->CXLReadSync(v_off, kValueSize, vbuf);
      V candidate;
      std::memcpy(&candidate, vbuf, sizeof(V));
      alignas(64) uint64_t keys2[kSlots];
      HT_STAT_INC(find_cxl_hr_reads);
      gDevice->CXLReadSync(hr_off, kHashRegionSize, keys2);
      if (keys2[0] != keys[0])
        return -1;
      if (key_traits_.match(candidate, key)) {
        val = candidate;
        // level==2 is old_BL during migration; its global_bucket_id collides
        // with new_BL's hint rows (same formula), so setting here would
        // poison the new_BL hint. Matches the convention in try_erase_from.
        if (set_hint && level != 2) {
          HT_STAT_INC(hint_set_populate_find);
          hint_set(global_bucket_id(level, bi), s, fp);
        }
        return 1;
      }
    }
    return 0;
  }

  template <typename WriteFn>
  bool update_impl(const K &key, WriteFn write_fn) {
    HT_STAT_INC(update_calls);
    uint64_t fp = make_fp(key);
    auto cands = get_candidates(key);
    HT_STAT_ADD(update_cand_probes, cands.count);
    bool contended[6] = {};
    bool old_bl_contended = false;
    bool searched[6] = {};

    auto try_bucket = [&](int c, bool set_hint = true) -> int {
      // Returns: 1 = updated (caller should return true), 0 = no match in
      // this bucket, -1 = contended (caller should fall through to the
      // contention-retry pass).
      uint64_t base = cands.base[c];
      size_t bi = cands.idx[c];
      uint64_t hr_off = base + bi * kBucketSize;
      alignas(64) uint64_t keys[kSlots];
      HT_STAT_INC(update_cxl_hr_reads);
      gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
      if (keys[0] & kDirtyBit) {
        contended[c] = true;
        if (cands.level[c] == 2)
          old_bl_contended = true;
        return -1;
      }
      if (find_fp_slot(keys, fp) < 0)
        return 0;
      int r = try_update_in(cands.level[c], base, bi, hr_off, keys, fp, key,
                            write_fn, set_hint);
      if (r == 1)
        return 1;
      if (r == -1) {
        contended[c] = true;
        if (cands.level[c] == 2)
          old_bl_contended = true;
        return -1;
      }
      return 0;
    };

#ifdef LEVEL_HASHTABLE_TAG_HINT
    TagHint *h = hint_;
    if (h) {
      for (int c = 0; c < cands.count; ++c) {
        if (!h->test(global_bucket_id(cands.level[c], cands.idx[c]), fp)) {
          HT_STAT_INC(update_phase1_hint_neg);
          continue;
        }
        HT_STAT_INC(update_phase1_hint_pos);
        searched[c] = true;
        int r = try_bucket(c, false);
        if (r == 1) {
          HT_STAT_INC(update_phase1_found);
          HT_STAT_INC(update_hits);
          return true;
        }
        if (r == 0)
          HT_STAT_INC(update_phase1_false_pos);
        // r == -1 is contended; deferred to the contention-retry pass.
      }
    }
#endif

    // Phase 2: fallback for hint-negative candidates (and for every candidate
    // when TAG_HINT is off). On success, try_update_in populates the hint so
    // subsequent finds/updates take the phase 1 fast path.
    for (int c = 0; c < cands.count; ++c) {
      if (searched[c])
        continue;
      HT_STAT_INC(update_phase2_reads);
      int r = try_bucket(c);
      if (r == 1) {
        HT_STAT_INC(update_phase2_found);
        HT_STAT_INC(update_hits);
        return true;
      }
    }

    for (int c = 0; c < cands.count; ++c) {
      if (!contended[c])
        continue;
      uint64_t base = cands.base[c];
      size_t bi = cands.idx[c];
      uint64_t hr_off = base + bi * kBucketSize;
      alignas(64) uint64_t keys[kSlots];
      HT_STAT_INC(update_phase3_retries);
      HT_STAT_INC(update_cxl_hr_reads);
      gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
      if (keys[0] & kDirtyBit) {
        if (!wait_for_unlock(hr_off, keys))
          continue;
      }
      for (;;) {
        if (find_fp_slot(keys, fp) < 0)
          break;
        int r = try_update_in(cands.level[c], base, bi, hr_off, keys, fp, key,
                              write_fn);
        if (r == 1) {
          HT_STAT_INC(update_hits);
          return true;
        }
        if (r == 0)
          break;
        // CAS contention: re-read and retry (no limit — update must not be
        // lost)
        _mm_pause();
        HT_STAT_INC(update_phase3_retries);
        HT_STAT_INC(update_cxl_hr_reads);
        gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
        if (keys[0] & kDirtyBit) {
          if (!wait_for_unlock(hr_off, keys))
            break;
        }
      }
    }

    if (old_bl_contended) {
      for (int c = 0; c < cands.count; ++c) {
        if (cands.level[c] == 2)
          continue;
        uint64_t base = level_offset(cands.level[c]);
        size_t bi = cands.idx[c];
        uint64_t hr_off = base + bi * kBucketSize;
        alignas(64) uint64_t keys[kSlots];
        HT_STAT_INC(update_cxl_hr_reads);
        gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
        if (keys[0] & kDirtyBit)
          continue;
        if (find_fp_slot(keys, fp) < 0)
          continue;
        int r = try_update_in(cands.level[c], base, bi, hr_off, keys, fp, key,
                              write_fn);
        if (r == 1) {
          HT_STAT_INC(update_hits);
          return true;
        }
      }
    }
    HT_STAT_INC(update_misses);
    return false;
  }

  template <typename WriteFn>
  int try_update_in(int level, uint64_t base, size_t bi, uint64_t hr_off,
                    uint64_t *keys, uint64_t fp, const K &key,
                    WriteFn &write_fn, bool set_hint = true) {
    uint64_t old;
    if (gDevice->CXLAtomicCasSync(hr_off, keys[0], make_locked(keys[0]),
                                  &old) != 0)
      return -1;
    int match = -1;
    V current;
    for (int s = 0; s < kSlots; ++s) {
      if (slot_fp(keys, s) != fp)
        continue;
      uint64_t v_off =
          base + bi * kBucketSize + kHashRegionSize + s * kValueSize;
      alignas(64) char vbuf[kValueSize];
      HT_STAT_INC(update_cxl_val_reads);
      gDevice->CXLReadSync(v_off, kValueSize, vbuf);
      std::memcpy(&current, vbuf, sizeof(V));
      if (key_traits_.match(current, key)) {
        match = s;
        break;
      }
    }
    if (match < 0) {
      HT_STAT_INC(update_cxl_hr_writes); // unlock_bucket
      unlock_bucket(hr_off, keys);
      return 0;
    }
    write_fn(current);
    uint64_t v_off =
        base + bi * kBucketSize + kHashRegionSize + match * kValueSize;
    alignas(64) char wbuf[kValueSize] = {};
    std::memcpy(wbuf, &current, sizeof(V));
    HT_STAT_INC(update_cxl_val_writes);
    gDevice->CXLWriteSync(v_off, kValueSize, wbuf);
    HT_STAT_INC(update_cxl_hr_writes); // unlock_bucket
    unlock_bucket(hr_off, keys);
    // Populate-on-write: refresh the tag for the matched slot. Phase 2 / the
    // contention retry paths reach here when the hint was stale (or empty),
    // so the next find for this key can resolve in phase 1. Skipped for
    // level==2 (old_BL) since its global_bucket_id aliases new_BL rows —
    // matches the convention in try_erase_from and scan_bucket_slots.
    if (level != 2) {
      HT_STAT_INC(hint_set_populate_update);
      hint_set(global_bucket_id(level, bi), match, fp);
    }
    return 1;
  }

  // ── Movement (paper-style try_movement + b2t_movement) ──
  //
  // Called after pass1/pass2 failed to place (key, val) directly into any
  // of the candidate buckets. Two phases, both faithful to Zuo et al.
  // ATC 2018 Level-Hashing:
  //
  //   Phase A (same-level 1-hop, try_movement):
  //     For each TL/BL candidate (TL-first), trylock it. For each
  //     occupied slot, compute the victim's alternate bucket on the SAME
  //     level (the non-source one of its two same-level candidates). If
  //     that alternate has space and can be trylocked, move the victim
  //     over and place (fp_new, val) in the vacated slot. Never TL→BL.
  //
  //   Phase B (BL→TL promotion, b2t_movement):
  //     For each BL candidate, trylock it. For each occupied slot,
  //     compute the victim's TWO TL candidates (h1 % tl_n_, h2 % tl_n_).
  //     Try each: if it has space and can be trylocked, promote the
  //     victim and place (fp_new, val) in the vacated BL slot.
  //
  // Takes the candidate set from ``insert_impl`` rather than recomputing
  // it locally so we see a consistent snapshot of migration state: if
  // migration is active, cands carries 6 entries (old_BL + TL + BL) and
  // we simply skip ``level == 2`` (old_BL) in both phases — matching the
  // same skip insert_impl's pass1/pass2 already apply. Paper-style
  // movement only touches TL/BL buckets, which is safe to run during an
  // active progressive rehash because the rehash sweep uses the same
  // bucket-level D-bit/version CAS protocol, so concurrent writers
  // serialize correctly.
  int try_migrate_and_insert(const K &key, const V &val, uint64_t fp_new,
                             const Candidates &cands) {
    // Phase A: same-level 1-hop. cands is ordered TL-first
    //   normal state: {TL[h1], TL[h2], BL[h1], BL[h2]}
    //   during mig:   {old_BL[h1], old_BL[h2], TL[h1], TL[h2], BL[h1], BL[h2]}
    // In both cases we skip level==2 and hit TL candidates before BL
    // candidates.
    // Locate the TL base from cands once — needed by b2t below. With cands
    // carrying a coherent layout snapshot, both phases address buckets that
    // are consistent with the (idx, level_n) used to compute candidates.
    uint64_t tl_base = 0;
    for (int i = 0; i < cands.count; ++i) {
      if (cands.level[i] == 0) {
        tl_base = cands.base[i];
        break;
      }
    }

    for (int c = 0; c < cands.count; ++c) {
      if (cands.level[c] == 2)
        continue;
      size_t level_n = (cands.level[c] == 0) ? cands.tl_n : cands.bl_n;
      if (same_level_movement_and_place(cands.level[c], cands.idx[c],
                                        cands.base[c], level_n, fp_new, val))
        return 1;
    }

    // Phase B: BL→TL promotion. Only BL candidates (level == 1) are
    // valid sources for b2t. Iterate cands to find them — works whether
    // cands.count is 4 (non-migration) or 6 (migration active).
    for (int c = 0; c < cands.count; ++c) {
      if (cands.level[c] != 1)
        continue;
      if (b2t_movement_and_place(cands.idx[c], cands.base[c], tl_base,
                                 cands.tl_n, fp_new, val))
        return 1;
    }

    return 0;
  }

  // Same-level 1-hop displacement. Lock bucket at (level, idx). For each
  // occupied slot, try to move its occupant to the victim's alternate
  // bucket on the SAME level. On first successful move, place (fp_new,
  // val) in the vacated slot and return true. On all-slot failure,
  // unlock the source bucket and return false. `base` and `level_n` come
  // from the caller's Candidates snapshot — must not be re-resolved here.
  bool same_level_movement_and_place(int level, size_t idx, uint64_t base,
                                     size_t level_n, uint64_t fp_new,
                                     const V &val) {
    uint64_t hr_off = base + idx * kBucketSize;

    alignas(64) uint64_t keys[kSlots];
    HT_STAT_INC(insert_cxl_hr_reads);
    gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
    if (keys[0] & kDirtyBit)
      return false;

    uint64_t old;
    if (gDevice->CXLAtomicCasSync(hr_off, keys[0], make_locked(keys[0]),
                                  &old) != 0)
      return false;

    for (int s = 0; s < kSlots; ++s) {
      uint64_t v_fp = slot_fp(keys, s);
      if (v_fp == 0)
        continue;

      uint64_t v_off =
          base + idx * kBucketSize + kHashRegionSize + s * kValueSize;
      alignas(64) char vbuf[kValueSize];
      HT_STAT_INC(insert_cxl_val_reads);
      gDevice->CXLReadSync(v_off, kValueSize, vbuf);
      V v_value;
      std::memcpy(&v_value, vbuf, sizeof(V));
      K v_key = key_traits_.extract(v_value);

      uint64_t v_raw = hasher_(v_key);
      uint64_t v_h1 = mix_h1(v_raw);
      uint64_t v_h2 = mix_h2(v_raw);
      // With F/S partition, src ∈ F-half means victim was placed at its
      // F-candidate; alt is its S-candidate (always in the S-half, so
      // jdx != idx by construction).
      size_t half = level_n / 2;
      size_t jdx = (idx < half) ? s_idx(v_h2, level_n) : f_idx(v_h1, level_n);
      if (jdx == idx)
        continue; // defensive: should be impossible with partition

      uint64_t alt_hr = base + jdx * kBucketSize;
      alignas(64) uint64_t alt_keys[kSlots];
      HT_STAT_INC(insert_cxl_hr_reads);
      gDevice->CXLReadSync(alt_hr, kHashRegionSize, alt_keys);
      if ((alt_keys[0] & kDirtyBit) || !has_empty_slot(alt_keys))
        continue;

      if (gDevice->CXLAtomicCasSync(alt_hr, alt_keys[0],
                                    make_locked(alt_keys[0]), &old) != 0)
        continue;

      int alt_target = find_empty_slot(alt_keys);
      if (alt_target < 0) {
        HT_STAT_INC(insert_cxl_hr_writes); // unlock_bucket
        unlock_bucket(alt_hr, alt_keys);
        continue;
      }

      // Move victim to the alternate bucket.
      uint64_t alt_v_off =
          base + jdx * kBucketSize + kHashRegionSize + alt_target * kValueSize;
      HT_STAT_INC(insert_cxl_val_writes);
      gDevice->CXLWriteSync(alt_v_off, kValueSize, vbuf);
      HT_STAT_INC(insert_cxl_hr_writes); // commit_bucket alt
      commit_bucket(alt_hr, alt_keys, alt_target, v_fp);

      // Place new entry in the vacated slot of the source bucket.
      alignas(64) char nbuf[kValueSize] = {};
      std::memcpy(nbuf, &val, sizeof(V));
      HT_STAT_INC(insert_cxl_val_writes);
      gDevice->CXLWriteSync(v_off, kValueSize, nbuf);
      HT_STAT_INC(insert_cxl_hr_writes); // commit_bucket src
      commit_bucket(hr_off, keys, s, fp_new);

      size_.fetch_add(1, std::memory_order_relaxed);
      HT_STAT_INC(hint_set_insert);
      hint_set(global_bucket_id(level, idx), s, fp_new);
      HT_STAT_INC(hint_set_insert);
      hint_set(global_bucket_id(level, jdx), alt_target, v_fp);
      return true;
    }

    HT_STAT_INC(insert_cxl_hr_writes); // unlock_bucket
    unlock_bucket(hr_off, keys);
    return false;
  }

  // BL→TL promotion. Lock BL bucket at bl_idx. For each occupied slot,
  // try to promote its occupant to one of its two TL candidates
  // (h1 % tl_n, h2 % tl_n). On first successful promotion, place
  // (fp_new, val) in the vacated BL slot and return true. On all-slot
  // failure, unlock the BL bucket and return false. bl_base/tl_base/tl_n
  // come from the caller's Candidates snapshot — must not be re-resolved.
  bool b2t_movement_and_place(size_t bl_idx, uint64_t bl_base, uint64_t tl_base,
                              size_t tl_n, uint64_t fp_new, const V &val) {
    uint64_t hr_off = bl_base + bl_idx * kBucketSize;

    alignas(64) uint64_t keys[kSlots];
    HT_STAT_INC(insert_cxl_hr_reads);
    gDevice->CXLReadSync(hr_off, kHashRegionSize, keys);
    if (keys[0] & kDirtyBit)
      return false;

    uint64_t old;
    if (gDevice->CXLAtomicCasSync(hr_off, keys[0], make_locked(keys[0]),
                                  &old) != 0)
      return false;

    for (int s = 0; s < kSlots; ++s) {
      uint64_t v_fp = slot_fp(keys, s);
      if (v_fp == 0)
        continue;

      uint64_t v_off =
          bl_base + bl_idx * kBucketSize + kHashRegionSize + s * kValueSize;
      alignas(64) char vbuf[kValueSize];
      HT_STAT_INC(insert_cxl_val_reads);
      gDevice->CXLReadSync(v_off, kValueSize, vbuf);
      V v_value;
      std::memcpy(&v_value, vbuf, sizeof(V));
      K v_key = key_traits_.extract(v_value);

      uint64_t v_raw = hasher_(v_key);
      uint64_t v_h1 = mix_h1(v_raw);
      uint64_t v_h2 = mix_h2(v_raw);
      size_t tl_indices[2] = {f_idx(v_h1, tl_n), s_idx(v_h2, tl_n)};

      for (int ti = 0; ti < 2; ++ti) {
        size_t tl_bi = tl_indices[ti];
        uint64_t tl_hr = tl_base + tl_bi * kBucketSize;
        alignas(64) uint64_t tl_keys[kSlots];
        HT_STAT_INC(insert_cxl_hr_reads);
        gDevice->CXLReadSync(tl_hr, kHashRegionSize, tl_keys);
        if ((tl_keys[0] & kDirtyBit) || !has_empty_slot(tl_keys))
          continue;

        if (gDevice->CXLAtomicCasSync(tl_hr, tl_keys[0],
                                      make_locked(tl_keys[0]), &old) != 0)
          continue;

        int tl_target = find_empty_slot(tl_keys);
        if (tl_target < 0) {
          HT_STAT_INC(insert_cxl_hr_writes); // unlock_bucket
          unlock_bucket(tl_hr, tl_keys);
          continue;
        }

        // Promote victim to TL.
        uint64_t tl_v_off = tl_base + tl_bi * kBucketSize + kHashRegionSize +
                            tl_target * kValueSize;
        HT_STAT_INC(insert_cxl_val_writes);
        gDevice->CXLWriteSync(tl_v_off, kValueSize, vbuf);
        HT_STAT_INC(insert_cxl_hr_writes); // commit_bucket tl
        commit_bucket(tl_hr, tl_keys, tl_target, v_fp);

        // Place new entry in the vacated BL slot.
        alignas(64) char nbuf[kValueSize] = {};
        std::memcpy(nbuf, &val, sizeof(V));
        HT_STAT_INC(insert_cxl_val_writes);
        gDevice->CXLWriteSync(v_off, kValueSize, nbuf);
        HT_STAT_INC(insert_cxl_hr_writes); // commit_bucket bl
        commit_bucket(hr_off, keys, s, fp_new);

        size_.fetch_add(1, std::memory_order_relaxed);
        HT_STAT_INC(hint_set_insert);
        hint_set(global_bucket_id(1, bl_idx), s, fp_new);
        HT_STAT_INC(hint_set_insert);
        hint_set(global_bucket_id(0, tl_bi), tl_target, v_fp);
        return true;
      }
    }

    HT_STAT_INC(insert_cxl_hr_writes); // unlock_bucket
    unlock_bucket(hr_off, keys);
    return false;
  }

  // ── Members ──
  std::atomic<uint64_t> tl_offset_{0};
  std::atomic<uint64_t> bl_offset_{0};
  std::atomic<size_t> tl_n_;
  std::atomic<size_t> bl_n_;
  std::atomic<bool> adopting_{false};
  // Bumped on every layout change (begin_migration/end_migration/adopt_layout).
  // Necessary because adopt_layout() rewrites tl_offset_/tl_n_/bl_offset_/bl_n_
  // *without* flipping migration_.active, so get_candidates()'s active-based
  // seqlock cannot detect a torn read of those fields. Public APIs sample
  // layout_epoch_ before/after the impl call; if it shifted and the op said
  // "not found", the Candidates were probably built from a half-updated
  // snapshot — retry with a fresh get_candidates().
  std::atomic<uint64_t> layout_epoch_{0};
  int n_mds_ = 1;
  mutable std::atomic<uint64_t> size_{0};
  Hash hasher_;
  KeyTraitsT key_traits_;
#ifdef LEVEL_HASHTABLE_TAG_HINT
  TagHint *hint_ = nullptr;
  TagHint *owned_hint_ = nullptr; // hint created by current/initial init
  TagHint *prev_hint_ = nullptr;  // hint from previous resize, deferred delete
#endif
  MigrationState migration_;

  // Resize state
  bool resize_enabled_ = false;
  std::atomic<bool> migration_begun_{false};
  ResizeConfig rcfg_{};
  LocalResizeCache resize_cache_;
};

} // namespace dfs
