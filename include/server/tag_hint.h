#pragma once

// TagHint: Local DRAM positive hint for TwoLevelHashtable find()
//
// Design: "Option C (local mirror)" — each bucket keeps an 8-entry tag array
// mirroring the slot layout of the hash region. Each tag is the low 32 bits
// of the key's fingerprint (60-bit fp), so a bucket costs 8 × 4B = 32B.
//
// For N = tl_n + bl_n ≈ 48K buckets: 48K × 32B = 1.5 MB per hashtable. All
// in local DRAM, no CXL traffic.
//
// Semantics:
//   - set(bucket_id, slot_idx, fp): record that this slot holds fp's low 32b
//   - clear(bucket_id, slot_idx):   zero the slot (insert/erase precise)
//   - test(bucket_id, fp):          true iff any of the 8 tags == low32(fp)
//
// Because clear() targets the exact slot, there is no bit accumulation under
// churn and no cross-fp false-negative hazard. The only false positive comes
// from two different 60-bit fps sharing the same low 32 bits in the same
// bucket (~1/2^32 per slot). Tag 0 is reserved as "empty", so we remap a
// low32 of 0 to 1 to avoid colliding with the empty sentinel.
//
// Per-MDS local state only: other MDS modifications don't update this hint,
// which just causes Phase 1 to miss and Phase 2 to kick in — no correctness
// issue, consistent with the previous design.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__AVX2__)
  #include <immintrin.h>
#endif

namespace dfs {

class TagHint {
public:
  static constexpr size_t kSlotsPerBucket = 8;

  explicit TagHint(size_t total_buckets)
      : n_(total_buckets), data_(total_buckets * kSlotsPerBucket, 0) {}

  // Record fp's low-32 tag in (bucket_id, slot_idx). Overwrites any prior
  // value in that slot — the caller guarantees the slot is the one just
  // committed in the hash region, so overwrite is the correct behavior.
  void set(size_t bucket_id, int slot_idx, uint64_t fp) {
    if (bucket_id >= n_ || slot_idx < 0 ||
        static_cast<size_t>(slot_idx) >= kSlotsPerBucket) {
      return;
    }
    data_[bucket_id * kSlotsPerBucket + slot_idx] = fp_to_tag(fp);
  }

  // Zero the tag at (bucket_id, slot_idx). Precise per-slot clear: no bit
  // accumulation, no aliasing with other fps in the same bucket.
  void clear(size_t bucket_id, int slot_idx) {
    if (bucket_id >= n_ || slot_idx < 0 ||
        static_cast<size_t>(slot_idx) >= kSlotsPerBucket) {
      return;
    }
    data_[bucket_id * kSlotsPerBucket + slot_idx] = 0;
  }

  // Does any slot in this bucket match fp's low 32 bits?
  // A bucket row is 8 × uint32_t = 256 bits, which fits exactly in one AVX2
  // register. On AVX2-capable targets we compare all 8 tags in parallel with
  // a single load + broadcast + cmpeq + testz (~4 cycles, branchless). This
  // replaces an 8-iteration early-exit loop whose mispredicted branches
  // dominate the per-find hint cost.
  bool test(size_t bucket_id, uint64_t fp) const {
    if (bucket_id >= n_) {
      return true; // conservative: fall through to Phase 2
    }
    const uint32_t tag = fp_to_tag(fp);
    const uint32_t *row = &data_[bucket_id * kSlotsPerBucket];
#if defined(__AVX2__)
    // std::vector<uint32_t> is not guaranteed 32-byte aligned, so use the
    // unaligned load — on Skylake+ it has the same throughput/latency as
    // the aligned form when the address is naturally aligned (which it is
    // here: each bucket row starts on a 32-byte boundary from data_[0]).
    const __m256i row_v =
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(row));
    const __m256i tag_v = _mm256_set1_epi32(static_cast<int>(tag));
    const __m256i eq = _mm256_cmpeq_epi32(row_v, tag_v);
    // testz(eq, eq) == 1 iff all lanes zero (no match). Invert for hit.
    return _mm256_testz_si256(eq, eq) == 0;
#else
    for (size_t i = 0; i < kSlotsPerBucket; ++i) {
      if (row[i] == tag) {
        return true;
      }
    }
    return false;
#endif
  }

  // Copy src[src_start .. src_start+count-1] into this[dst_start..], slot
  // array per bucket included. Used during resize to carry over the hint
  // range that still maps to the same bucket index.
  void copy_from(const TagHint &src, size_t src_start, size_t dst_start,
                 size_t count) {
    for (size_t i = 0; i < count; ++i) {
      size_t si = src_start + i, di = dst_start + i;
      if (si < src.n_ && di < n_) {
        const uint32_t *sp = &src.data_[si * kSlotsPerBucket];
        uint32_t *dp = &data_[di * kSlotsPerBucket];
        for (size_t s = 0; s < kSlotsPerBucket; ++s) {
          dp[s] = sp[s];
        }
      }
    }
  }

  size_t size() const { return n_; }

  void reset() { std::fill(data_.begin(), data_.end(), 0u); }

  size_t memory_bytes() const {
    return n_ * kSlotsPerBucket * sizeof(uint32_t);
  }

private:
  // Truncate to low 32 bits. Remap 0 → 1 so the sentinel "empty slot" (0)
  // is never confused with a real fp whose low 32 bits happen to be zero.
  // The 60-bit fp itself is already non-zero (make_fp guarantees that), so
  // at most one distinct fp value is affected.
  static uint32_t fp_to_tag(uint64_t fp) {
    uint32_t t = static_cast<uint32_t>(fp);
    return t == 0 ? 1u : t;
  }

  size_t n_;
  std::vector<uint32_t> data_; // n_ * kSlotsPerBucket entries
};

} // namespace dfs
