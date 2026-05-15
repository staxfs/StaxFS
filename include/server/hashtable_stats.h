#pragma once

// Hashtable Stats — Opt-in counters for TwoLevelHashtable op paths.
//
// Enabled by defining LEVEL_HASHTABLE_STATS in include/common/metadata_types.h.
// When disabled, all macros expand to no-ops and incur zero runtime cost.
//
// Purpose: measure the effect of LEVEL_HASHTABLE_TAG_HINT on every path that
// touches the hashtable — how many CXL bytes does the hint save per op, and
// at what false-positive cost? Compare builds with TAG_HINT on vs off.
//
// Op groups covered (symmetric layout per group where applicable):
//   - find   (hint-guided lookup → phase 1 / phase 2 fallback / phase 3 retry)
//   - insert (no hint for lookup; tracks pass1 / pass2 / cuckoo / migrate)
//   - update (hint-guided matching + write-through, phase 1/2/3 mirror)
//   - erase  (hint-guided matching + HR clear, phase 1/2/3 mirror)
//
// Raw CXL traffic counters per op group:
//   - cxl_hr_reads   : 64B hash-region reads
//   - cxl_hr_writes  : 64B hash-region writes (commit / unlock / slot clear)
//   - cxl_val_reads  : 128B value reads
//   - cxl_val_writes : 128B value writes
// bytes/op = (hr_reads + hr_writes) * 64 + (val_reads + val_writes) * 128
//
// Hint maintenance is split by source so populate-on-read / populate-on-write
// can be distinguished from the real insert / erase activity.
//
// Usage: dump + reset from Metadata::PrintSpace() at the end of a workload.
// Counters are atomic, so safe under concurrent ops.

#include "common/metadata_types.h"
#include <atomic>
#include <cstdint>

#ifdef LEVEL_HASHTABLE_STATS
  #include <spdlog/spdlog.h>
#endif

namespace dfs {

struct HashtableStats {
  // ── FIND ─────────────────────────────────────────────────────────
  std::atomic<uint64_t> find_calls{0};
  std::atomic<uint64_t> find_hits{0};
  std::atomic<uint64_t> find_misses{0};
  std::atomic<uint64_t> find_cand_probes{0};
  std::atomic<uint64_t> find_phase1_hint_neg{0};
  std::atomic<uint64_t> find_phase1_hint_pos{0};
  std::atomic<uint64_t> find_phase1_found{0};
  std::atomic<uint64_t> find_phase1_false_pos{0};
  std::atomic<uint64_t> find_phase2_reads{0};
  std::atomic<uint64_t> find_phase2_found{0};
  std::atomic<uint64_t> find_phase3_retries{0};
  std::atomic<uint64_t> find_cxl_hr_reads{0};
  std::atomic<uint64_t> find_cxl_hr_writes{0}; // always 0 (find is read-only)
  std::atomic<uint64_t> find_cxl_val_reads{0};
  std::atomic<uint64_t> find_cxl_val_writes{0}; // always 0

  // ── INSERT (no hint-guided lookup; looks for empty slots, not fp match) ──
  std::atomic<uint64_t> insert_calls{0};
  std::atomic<uint64_t> insert_hits{0};   // placed (pass1 / pass2 / cuckoo)
  std::atomic<uint64_t> insert_misses{0}; // ran out of slots, triggered resize
  std::atomic<uint64_t> insert_cand_probes{0};
  std::atomic<uint64_t> insert_pass1_reads{0};
  std::atomic<uint64_t> insert_pass1_placed{0};
  std::atomic<uint64_t> insert_pass2_reads{0};
  std::atomic<uint64_t> insert_pass2_placed{0};
  std::atomic<uint64_t> insert_cuckoo_calls{0};
  std::atomic<uint64_t> insert_cuckoo_placed{0};
  std::atomic<uint64_t> insert_cxl_hr_reads{0};
  std::atomic<uint64_t> insert_cxl_hr_writes{0};
  std::atomic<uint64_t> insert_cxl_val_reads{0};
  std::atomic<uint64_t> insert_cxl_val_writes{0};

  // ── UPDATE ───────────────────────────────────────────────────────
  std::atomic<uint64_t> update_calls{0};
  std::atomic<uint64_t> update_hits{0};
  std::atomic<uint64_t> update_misses{0};
  std::atomic<uint64_t> update_cand_probes{0};
  std::atomic<uint64_t> update_phase1_hint_neg{0};
  std::atomic<uint64_t> update_phase1_hint_pos{0};
  std::atomic<uint64_t> update_phase1_found{0};
  std::atomic<uint64_t> update_phase1_false_pos{0};
  std::atomic<uint64_t> update_phase2_reads{0};
  std::atomic<uint64_t> update_phase2_found{0};
  std::atomic<uint64_t> update_phase3_retries{0};
  std::atomic<uint64_t> update_cxl_hr_reads{0};
  std::atomic<uint64_t> update_cxl_hr_writes{0};
  std::atomic<uint64_t> update_cxl_val_reads{0};
  std::atomic<uint64_t> update_cxl_val_writes{0};

  // ── ERASE ────────────────────────────────────────────────────────
  std::atomic<uint64_t> erase_calls{0};
  std::atomic<uint64_t> erase_hits{0};
  std::atomic<uint64_t> erase_misses{0};
  std::atomic<uint64_t> erase_cand_probes{0};
  std::atomic<uint64_t> erase_phase1_hint_neg{0};
  std::atomic<uint64_t> erase_phase1_hint_pos{0};
  std::atomic<uint64_t> erase_phase1_found{0};
  std::atomic<uint64_t> erase_phase1_false_pos{0};
  std::atomic<uint64_t> erase_phase2_reads{0};
  std::atomic<uint64_t> erase_phase2_found{0};
  std::atomic<uint64_t> erase_phase3_retries{0};
  std::atomic<uint64_t> erase_cxl_hr_reads{0};
  std::atomic<uint64_t> erase_cxl_hr_writes{0};
  std::atomic<uint64_t> erase_cxl_val_reads{0};
  std::atomic<uint64_t> erase_cxl_val_writes{0}; // always 0

  // ── HINT MAINTENANCE (split by source) ───────────────────────────
  std::atomic<uint64_t> hint_set_insert{0};          // try_insert_into + cuckoo
  std::atomic<uint64_t> hint_set_migrate{0};         // insert_to_new_layout
  std::atomic<uint64_t> hint_set_populate_find{0};   // scan_bucket_slots
  std::atomic<uint64_t> hint_set_populate_update{0}; // try_update_in
  std::atomic<uint64_t> hint_clear_erase{0};         // try_erase_from
};

inline HashtableStats gHashtableStats;

#ifdef LEVEL_HASHTABLE_STATS

// X-macro: the single source of truth for the field list. Reset + Dump both
// iterate over it, so adding/removing a field only touches one place.
  #define HT_STAT_FIELDS(X)                                                    \
    X(find_calls)                                                              \
    X(find_hits)                                                               \
    X(find_misses)                                                             \
    X(find_cand_probes)                                                        \
    X(find_phase1_hint_neg)                                                    \
    X(find_phase1_hint_pos)                                                    \
    X(find_phase1_found)                                                       \
    X(find_phase1_false_pos)                                                   \
    X(find_phase2_reads)                                                       \
    X(find_phase2_found)                                                       \
    X(find_phase3_retries)                                                     \
    X(find_cxl_hr_reads)                                                       \
    X(find_cxl_hr_writes)                                                      \
    X(find_cxl_val_reads)                                                      \
    X(find_cxl_val_writes)                                                     \
    X(insert_calls)                                                            \
    X(insert_hits)                                                             \
    X(insert_misses)                                                           \
    X(insert_cand_probes)                                                      \
    X(insert_pass1_reads)                                                      \
    X(insert_pass1_placed)                                                     \
    X(insert_pass2_reads)                                                      \
    X(insert_pass2_placed)                                                     \
    X(insert_cuckoo_calls)                                                     \
    X(insert_cuckoo_placed)                                                    \
    X(insert_cxl_hr_reads)                                                     \
    X(insert_cxl_hr_writes)                                                    \
    X(insert_cxl_val_reads)                                                    \
    X(insert_cxl_val_writes)                                                   \
    X(update_calls)                                                            \
    X(update_hits)                                                             \
    X(update_misses)                                                           \
    X(update_cand_probes)                                                      \
    X(update_phase1_hint_neg)                                                  \
    X(update_phase1_hint_pos)                                                  \
    X(update_phase1_found)                                                     \
    X(update_phase1_false_pos)                                                 \
    X(update_phase2_reads)                                                     \
    X(update_phase2_found)                                                     \
    X(update_phase3_retries)                                                   \
    X(update_cxl_hr_reads)                                                     \
    X(update_cxl_hr_writes)                                                    \
    X(update_cxl_val_reads)                                                    \
    X(update_cxl_val_writes)                                                   \
    X(erase_calls)                                                             \
    X(erase_hits)                                                              \
    X(erase_misses)                                                            \
    X(erase_cand_probes)                                                       \
    X(erase_phase1_hint_neg)                                                   \
    X(erase_phase1_hint_pos)                                                   \
    X(erase_phase1_found)                                                      \
    X(erase_phase1_false_pos)                                                  \
    X(erase_phase2_reads)                                                      \
    X(erase_phase2_found)                                                      \
    X(erase_phase3_retries)                                                    \
    X(erase_cxl_hr_reads)                                                      \
    X(erase_cxl_hr_writes)                                                     \
    X(erase_cxl_val_reads)                                                     \
    X(erase_cxl_val_writes)                                                    \
    X(hint_set_insert)                                                         \
    X(hint_set_migrate)                                                        \
    X(hint_set_populate_find)                                                  \
    X(hint_set_populate_update)                                                \
    X(hint_clear_erase)

inline void HashtableStatsReset() {
  auto &s = gHashtableStats;
  #define HT_STAT_RESET_ONE(name) s.name.store(0, std::memory_order_relaxed);
  HT_STAT_FIELDS(HT_STAT_RESET_ONE)
  #undef HT_STAT_RESET_ONE
}

inline double bytes_per_op(uint64_t hr_r, uint64_t hr_w, uint64_t v_r,
                           uint64_t v_w, uint64_t calls) {
  if (!calls)
    return 0.0;
  return (static_cast<double>(hr_r + hr_w) * 64.0 +
          static_cast<double>(v_r + v_w) * 128.0) /
         static_cast<double>(calls);
}

inline void HashtableStatsDump() {
  auto &s = gHashtableStats;
  #define HT_STAT_LOAD(name)                                                   \
    uint64_t name = s.name.load(std::memory_order_relaxed);
  HT_STAT_FIELDS(HT_STAT_LOAD)
  #undef HT_STAT_LOAD

  SPDLOG_INFO("── HashtableStats ──");
  #ifdef LEVEL_HASHTABLE_TAG_HINT
  SPDLOG_INFO("  build: TAG_HINT=ON");
  #else
  SPDLOG_INFO("  build: TAG_HINT=OFF");
  #endif

  // ─ FIND ───────────────────────────────────────────────────────
  SPDLOG_INFO("  find_calls              = {}", find_calls);
  SPDLOG_INFO("  find_hits               = {}", find_hits);
  SPDLOG_INFO("  find_misses             = {}", find_misses);
  SPDLOG_INFO("  find_cand_probes        = {}", find_cand_probes);
  SPDLOG_INFO("  find_phase1_hint_neg    = {}", find_phase1_hint_neg);
  SPDLOG_INFO("  find_phase1_hint_pos    = {}", find_phase1_hint_pos);
  SPDLOG_INFO("  find_phase1_found       = {}", find_phase1_found);
  SPDLOG_INFO("  find_phase1_false_pos   = {}", find_phase1_false_pos);
  SPDLOG_INFO("  find_phase2_reads       = {}", find_phase2_reads);
  SPDLOG_INFO("  find_phase2_found       = {}", find_phase2_found);
  SPDLOG_INFO("  find_phase3_retries     = {}", find_phase3_retries);
  SPDLOG_INFO("  find_cxl_hr_reads       = {}", find_cxl_hr_reads);
  SPDLOG_INFO("  find_cxl_hr_writes      = {}", find_cxl_hr_writes);
  SPDLOG_INFO("  find_cxl_val_reads      = {}", find_cxl_val_reads);
  SPDLOG_INFO("  find_cxl_val_writes     = {}", find_cxl_val_writes);
  if (find_calls) {
    SPDLOG_INFO("  find_bytes_per_op       = {:.1f}",
                bytes_per_op(find_cxl_hr_reads, find_cxl_hr_writes,
                             find_cxl_val_reads, find_cxl_val_writes,
                             find_calls));
  }

  // ─ INSERT ─────────────────────────────────────────────────────
  SPDLOG_INFO("  insert_calls            = {}", insert_calls);
  SPDLOG_INFO("  insert_hits             = {}", insert_hits);
  SPDLOG_INFO("  insert_misses           = {}", insert_misses);
  SPDLOG_INFO("  insert_cand_probes      = {}", insert_cand_probes);
  SPDLOG_INFO("  insert_pass1_reads      = {}", insert_pass1_reads);
  SPDLOG_INFO("  insert_pass1_placed     = {}", insert_pass1_placed);
  SPDLOG_INFO("  insert_pass2_reads      = {}", insert_pass2_reads);
  SPDLOG_INFO("  insert_pass2_placed     = {}", insert_pass2_placed);
  SPDLOG_INFO("  insert_cuckoo_calls     = {}", insert_cuckoo_calls);
  SPDLOG_INFO("  insert_cuckoo_placed    = {}", insert_cuckoo_placed);
  SPDLOG_INFO("  insert_cxl_hr_reads     = {}", insert_cxl_hr_reads);
  SPDLOG_INFO("  insert_cxl_hr_writes    = {}", insert_cxl_hr_writes);
  SPDLOG_INFO("  insert_cxl_val_reads    = {}", insert_cxl_val_reads);
  SPDLOG_INFO("  insert_cxl_val_writes   = {}", insert_cxl_val_writes);
  if (insert_calls) {
    SPDLOG_INFO("  insert_bytes_per_op     = {:.1f}",
                bytes_per_op(insert_cxl_hr_reads, insert_cxl_hr_writes,
                             insert_cxl_val_reads, insert_cxl_val_writes,
                             insert_calls));
  }

  // ─ UPDATE ─────────────────────────────────────────────────────
  SPDLOG_INFO("  update_calls            = {}", update_calls);
  SPDLOG_INFO("  update_hits             = {}", update_hits);
  SPDLOG_INFO("  update_misses           = {}", update_misses);
  SPDLOG_INFO("  update_cand_probes      = {}", update_cand_probes);
  SPDLOG_INFO("  update_phase1_hint_neg  = {}", update_phase1_hint_neg);
  SPDLOG_INFO("  update_phase1_hint_pos  = {}", update_phase1_hint_pos);
  SPDLOG_INFO("  update_phase1_found     = {}", update_phase1_found);
  SPDLOG_INFO("  update_phase1_false_pos = {}", update_phase1_false_pos);
  SPDLOG_INFO("  update_phase2_reads     = {}", update_phase2_reads);
  SPDLOG_INFO("  update_phase2_found     = {}", update_phase2_found);
  SPDLOG_INFO("  update_phase3_retries   = {}", update_phase3_retries);
  SPDLOG_INFO("  update_cxl_hr_reads     = {}", update_cxl_hr_reads);
  SPDLOG_INFO("  update_cxl_hr_writes    = {}", update_cxl_hr_writes);
  SPDLOG_INFO("  update_cxl_val_reads    = {}", update_cxl_val_reads);
  SPDLOG_INFO("  update_cxl_val_writes   = {}", update_cxl_val_writes);
  if (update_calls) {
    SPDLOG_INFO("  update_bytes_per_op     = {:.1f}",
                bytes_per_op(update_cxl_hr_reads, update_cxl_hr_writes,
                             update_cxl_val_reads, update_cxl_val_writes,
                             update_calls));
  }

  // ─ ERASE ──────────────────────────────────────────────────────
  SPDLOG_INFO("  erase_calls             = {}", erase_calls);
  SPDLOG_INFO("  erase_hits              = {}", erase_hits);
  SPDLOG_INFO("  erase_misses            = {}", erase_misses);
  SPDLOG_INFO("  erase_cand_probes       = {}", erase_cand_probes);
  SPDLOG_INFO("  erase_phase1_hint_neg   = {}", erase_phase1_hint_neg);
  SPDLOG_INFO("  erase_phase1_hint_pos   = {}", erase_phase1_hint_pos);
  SPDLOG_INFO("  erase_phase1_found      = {}", erase_phase1_found);
  SPDLOG_INFO("  erase_phase1_false_pos  = {}", erase_phase1_false_pos);
  SPDLOG_INFO("  erase_phase2_reads      = {}", erase_phase2_reads);
  SPDLOG_INFO("  erase_phase2_found      = {}", erase_phase2_found);
  SPDLOG_INFO("  erase_phase3_retries    = {}", erase_phase3_retries);
  SPDLOG_INFO("  erase_cxl_hr_reads      = {}", erase_cxl_hr_reads);
  SPDLOG_INFO("  erase_cxl_hr_writes     = {}", erase_cxl_hr_writes);
  SPDLOG_INFO("  erase_cxl_val_reads     = {}", erase_cxl_val_reads);
  SPDLOG_INFO("  erase_cxl_val_writes    = {}", erase_cxl_val_writes);
  if (erase_calls) {
    SPDLOG_INFO("  erase_bytes_per_op      = {:.1f}",
                bytes_per_op(erase_cxl_hr_reads, erase_cxl_hr_writes,
                             erase_cxl_val_reads, erase_cxl_val_writes,
                             erase_calls));
  }

  // ─ HINT MAINTENANCE ───────────────────────────────────────────
  SPDLOG_INFO("  hint_set_insert         = {}", hint_set_insert);
  SPDLOG_INFO("  hint_set_migrate        = {}", hint_set_migrate);
  SPDLOG_INFO("  hint_set_populate_find  = {}", hint_set_populate_find);
  SPDLOG_INFO("  hint_set_populate_update= {}", hint_set_populate_update);
  SPDLOG_INFO("  hint_clear_erase        = {}", hint_clear_erase);
}

  #define HT_STAT_INC(name)                                                    \
    dfs::gHashtableStats.name.fetch_add(1, std::memory_order_relaxed)
  #define HT_STAT_ADD(name, n)                                                 \
    dfs::gHashtableStats.name.fetch_add((n), std::memory_order_relaxed)

#else // LEVEL_HASHTABLE_STATS not defined

inline void HashtableStatsReset() {}

inline void HashtableStatsDump() {}

  #define HT_STAT_INC(name) ((void)0)
  #define HT_STAT_ADD(name, n) ((void)0)

#endif

} // namespace dfs
