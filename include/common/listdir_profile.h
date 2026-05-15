#pragma once

// Listdir latency profiler — compile-time gated per-step instrumentation.
//
// Enable by defining LISTDIR_LATENCY_PROFILE in metadata_types.h (or at the
// compiler command line). When disabled, every macro expands to `(void)0` so
// the instrumentation has exactly zero runtime and binary footprint.
//
// Coverage:
//   - Client: dfs_opendir → OpenDir RPC → LoadMergedDirents (per-meta
//     GetDentViews RPC fan-out, buffer merge, sort/pack, alloc+memcpy).
//   - Server: MDFDCommonReqHandler → AcquireBuffer → Get{Dents,DentViews} →
//     CollectPersistentDentViews (ReadDirLatest, WAL tail scan, names sort,
//     emplace) → SerializeDent{Views,s} → response encode.
//
// Each instrumented site uses an RAII scoped timer that atomically
// accumulates count/total_ns/max_ns into a process-global singleton.
// Dump is triggered from:
//   - Server: Metadata::PrintSpace() (already runs periodically).
//   - Client: dfs_closedir() every N calls (configurable below).
//
// Dumps are cumulative across the whole process lifetime (no reset); derive
// per-interval rates from successive dumps if needed.

#include "common/metadata_types.h"

#ifdef LISTDIR_LATENCY_PROFILE

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <spdlog/spdlog.h>

namespace dfs {

enum ListdirStep : int {
  // ─── Client ────────────────────────────────────────────────────
  kCliOpendirTotal = 0,       // whole dfs_opendir body
  kCliOpendirRpc,             // OpenDir path-walk RPC
  kCliLoadMergedTotal,        // whole LoadMergedDirents body
  kCliGetDentViewsRpc,        // one GetDentViews RPC (per meta, per page)
  kCliMergeBuffer,            // MergeDentViewsFromBuffer call
  kCliSortAndPack,            // names vector + std::sort + pack loop
  kCliAllocMergedBuf,         // final malloc + memcpy into ClientDirstream

  // ─── Server ────────────────────────────────────────────────────
  kSrvFdHandlerTotal,         // whole MDFDCommonReqHandler body
  kSrvAcquireBuffer,          // AcquireBuffer spin+sleep cost
  kSrvGetDentViewsOuter,      // GetDentViews wrapper (vector + serialize)
  kSrvCollectDentViews,       // CollectPersistentDentViews full body
  kSrvReadDirLatest,          // SSDDentRegion::ReadDirLatest call only
  kSrvWalTailScan,            // WAL tail scan + merge loop
  kSrvNamesBuildSort,         // names vector + std::sort
  kSrvEntriesEmplace,         // per-name emplace into DentView vector
  kSrvSerializeDentViews,     // SerializeDentViews call
  kSrvRpcResponseEncode,      // response buffer alloc + memcpy + enqueue

  kListdirStepCount,
};

inline const char *ListdirStepName(int s) {
  static constexpr const char *kNames[] = {
      "cli.opendir_total",
      "cli.opendir_rpc",
      "cli.load_merged_total",
      "cli.get_dent_views_rpc",
      "cli.merge_buffer",
      "cli.sort_and_pack",
      "cli.alloc_merged_buf",
      "srv.fd_handler_total",
      "srv.acquire_buffer",
      "srv.get_dent_views_outer",
      "srv.collect_dent_views",
      "srv.read_dir_latest",
      "srv.wal_tail_scan",
      "srv.names_build_sort",
      "srv.entries_emplace",
      "srv.serialize_dent_views",
      "srv.rpc_response_encode",
  };
  return (s >= 0 && s < kListdirStepCount) ? kNames[s] : "?";
}

struct ListdirStepStats {
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> total_ns{0};
  std::atomic<uint64_t> max_ns{0};
};

class ListdirProfiler {
public:
  static ListdirProfiler &Instance() {
    static ListdirProfiler inst;
    return inst;
  }

  void Record(int step, uint64_t ns) {
    if (step < 0 || step >= kListdirStepCount) {
      return;
    }
    auto &s = stats_[step];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.total_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t cur = s.max_ns.load(std::memory_order_relaxed);
    while (ns > cur && !s.max_ns.compare_exchange_weak(
                           cur, ns, std::memory_order_relaxed)) {
      // retry
    }
  }

  // Dump cumulative stats. Safe to call concurrently with Record(); the
  // snapshot may show a slightly inconsistent (count, total) pair but the
  // per-field values are monotonic.
  void Dump(const char *tag) {
    SPDLOG_INFO("── listdir profile ({}) ──", tag);
    SPDLOG_INFO("  {:<28}  {:>12}  {:>12}  {:>12}  {:>14}", "step", "count",
                "avg_us", "max_us", "total_ms");
    bool any = false;
    for (int i = 0; i < kListdirStepCount; ++i) {
      uint64_t cnt = stats_[i].count.load(std::memory_order_relaxed);
      if (cnt == 0) {
        continue;
      }
      any = true;
      uint64_t tot = stats_[i].total_ns.load(std::memory_order_relaxed);
      uint64_t mx = stats_[i].max_ns.load(std::memory_order_relaxed);
      double avg_us = static_cast<double>(tot) / static_cast<double>(cnt) /
                      1000.0;
      double max_us = static_cast<double>(mx) / 1000.0;
      double tot_ms = static_cast<double>(tot) / 1.0e6;
      SPDLOG_INFO("  {:<28}  {:>12}  {:>12.2f}  {:>12.2f}  {:>14.2f}",
                  ListdirStepName(i), cnt, avg_us, max_us, tot_ms);
    }
    if (!any) {
      SPDLOG_INFO("  (no samples)");
    }
  }

private:
  ListdirProfiler() = default;
  std::array<ListdirStepStats, kListdirStepCount> stats_;
};

class ListdirScopedTimer {
public:
  explicit ListdirScopedTimer(int step)
      : step_(step), start_(std::chrono::steady_clock::now()) {}
  ~ListdirScopedTimer() {
    auto end = std::chrono::steady_clock::now();
    uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_)
            .count());
    ListdirProfiler::Instance().Record(step_, ns);
  }
  ListdirScopedTimer(const ListdirScopedTimer &) = delete;
  ListdirScopedTimer &operator=(const ListdirScopedTimer &) = delete;

private:
  int step_;
  std::chrono::steady_clock::time_point start_;
};

// Counter for dfs_closedir-driven periodic dump on the client side.
inline std::atomic<uint64_t> &ListdirProfileDumpCounter() {
  static std::atomic<uint64_t> counter{0};
  return counter;
}

// Dump every N closedir calls. Cheap branch on the hot path.
inline void ListdirProfileMaybeDump(uint64_t interval, const char *tag) {
  if (interval == 0) {
    return;
  }
  uint64_t n =
      ListdirProfileDumpCounter().fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % interval == 0) {
    ListdirProfiler::Instance().Dump(tag);
  }
}

} // namespace dfs

#define LISTDIR_PROFILE_CONCAT_(a, b) a##b
#define LISTDIR_PROFILE_CONCAT(a, b) LISTDIR_PROFILE_CONCAT_(a, b)

// RAII scope timer. Place at the top of a scope to time the whole scope.
#define LISTDIR_PROFILE_SCOPE(step_enum)                                       \
  ::dfs::ListdirScopedTimer LISTDIR_PROFILE_CONCAT(                            \
      _listdir_prof_timer_, __COUNTER__)((step_enum))

// Dump cumulative stats now. `tag` is free-form, e.g. "server-periodic".
#define LISTDIR_PROFILE_DUMP(tag) ::dfs::ListdirProfiler::Instance().Dump(tag)

// Increment the closedir-driven counter and dump every `interval` calls.
#define LISTDIR_PROFILE_MAYBE_DUMP(interval, tag)                              \
  ::dfs::ListdirProfileMaybeDump((interval), (tag))

#else // !LISTDIR_LATENCY_PROFILE

#define LISTDIR_PROFILE_SCOPE(step_enum) ((void)0)
#define LISTDIR_PROFILE_DUMP(tag) ((void)0)
#define LISTDIR_PROFILE_MAYBE_DUMP(interval, tag) ((void)0)

#endif // LISTDIR_LATENCY_PROFILE
