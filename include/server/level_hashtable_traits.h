#pragma once

// Traits / typedef glue for the two hashtable implementations that
// dfs-prototype can use as its dirent+inode store:
//
//   • default:                     TwoLevelHashtable (optimized, progressive
//                                   resize, TagHint, 8-slot buckets)
//   • USING_LEVEL_HASHTABLE_BASELINE on:   ConcurrentLevelHashtable
//   (paper-faithful
//                                   Level-Hashing baseline, stop-the-world
//                                   resize, 4-slot buckets). Benchmark use
//                                   only.
//
// The toggle lives in `common/metadata_types.h`. Flip it by
// uncommenting / re-commenting; full rebuild; nothing else changes.
//
// NOTE ON INCLUDES: we unconditionally pull in `server/level_hashtable.h`
// even when the baseline macro is on. Rationale: `metadata.cc` uses the
// symbol `dfs::ResizeMeta` for aggregate-init of the 64 B CXL resize meta
// region, and that type lives in `level_hashtable.h`. In baseline mode
// the `TwoLevelHashtable` class template is defined but never
// instantiated, so the only cost is compile-time header parsing —
// there is no runtime overhead and the on-CXL layout is byte-identical
// to the baseline's own `CLHResizeMeta`. This is a pragmatic deviation
// from task_plan.md §2 that keeps `metadata.cc` byte-identical.

#include "common/metadata_types.h"
#include "server/level_hashtable.h"

#ifdef USING_LEVEL_HASHTABLE_BASELINE
  #include "server/concurrent_level_hashtable.h"
#endif

#include <cstring>
#include <string>
#include <utility>

namespace dfs {

// DKeyPair = (parent_id, filename). Redeclared here to keep the traits
// header self-contained relative to `metadata.h`.
using DKeyPair = std::pair<uint64_t, std::string>;

#ifndef USING_LEVEL_HASHTABLE_BASELINE
// ──────────────────────────────────────────────────────────────────
// Optimized path: specialize TwoLevelHashtable's `KeyTraits`.
// ──────────────────────────────────────────────────────────────────

template <>
struct KeyTraits<DKeyPair, Dirent> {
  bool match(const Dirent &d, const DKeyPair &key) const {
    return d.pid_ == key.first &&
           std::strncmp(d.name_, key.second.c_str(), sizeof(d.name_)) == 0;
  }

  DKeyPair extract(const Dirent &d) const {
    return {d.pid_, std::string(d.name_)};
  }
};

template <>
struct KeyTraits<uint64_t, Inode> {
  bool match(const Inode &inode, uint64_t key) const {
    return inode.id_ == key;
  }

  uint64_t extract(const Inode &inode) const { return inode.id_; }
};

#else
// ──────────────────────────────────────────────────────────────────
// Baseline path: specialize ConcurrentLevelHashtable's `CLHKeyTraits`.
// The bodies are byte-for-byte copies of the optimized versions above
// (decision #4: no code sharing between the two implementations).
// ──────────────────────────────────────────────────────────────────

template <>
struct CLHKeyTraits<DKeyPair, Dirent> {
  bool match(const Dirent &d, const DKeyPair &key) const {
    return d.pid_ == key.first &&
           std::strncmp(d.name_, key.second.c_str(), sizeof(d.name_)) == 0;
  }

  DKeyPair extract(const Dirent &d) const {
    return {d.pid_, std::string(d.name_)};
  }
};

template <>
struct CLHKeyTraits<uint64_t, Inode> {
  bool match(const Inode &inode, uint64_t key) const {
    return inode.id_ == key;
  }

  uint64_t extract(const Inode &inode) const { return inode.id_; }
};

#endif // USING_LEVEL_HASHTABLE_BASELINE

// ── Hashers (shared across both implementations) ──
struct PairHash_LH {
  auto operator()(const DKeyPair &p) const -> size_t {
    auto h1 = std::hash<uint64_t>{}(p.first);
    auto h2 = std::hash<std::string>{}(p.second);
    h1 ^= h2 + 0x517cc1b727220a95ULL + (h1 << 6) + (h1 >> 2);
    return h1;
  }
};

struct IDHash_LH {
  auto operator()(uint64_t p) const -> size_t { return p; }
};

// ── Public typedefs — the only thing callers actually see ──
#ifndef USING_LEVEL_HASHTABLE_BASELINE
using DirentHashtable = TwoLevelHashtable<DKeyPair, Dirent, PairHash_LH>;
using InodeHashtable = TwoLevelHashtable<uint64_t, Inode, IDHash_LH>;
#else
using DirentHashtable = ConcurrentLevelHashtable<DKeyPair, Dirent, PairHash_LH>;
using InodeHashtable = ConcurrentLevelHashtable<uint64_t, Inode, IDHash_LH>;
#endif

} // namespace dfs
