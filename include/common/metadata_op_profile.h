#pragma once

// Metadata-op latency profiler — compile-time gated.
//
// Enable by defining MD_OP_LATENCY_PROFILE in metadata_types.h. When
// disabled, every macro expands to `(void)0` so the instrumentation has
// exactly zero runtime and binary footprint.
//
// Coverage: the 8 path-common metadata ops handled by
// MDPathCommonReqHandler — Unlink/RemoveDir/Access/MakeDir/Stat/Rename/
// Link/Create.
//
// Per-op storage: 64 atomic log2 buckets (bucket k counts samples in
// [2^k, 2^(k+1)) ns) plus atomic count and total_ns. Lock-free record,
// fixed memory, no sample cap. Percentiles are bucket-midpoint
// approximations (within 50% relative error of true value).
//
// Dump from Metadata::PrintSpace() (already runs periodically). Stats are
// cumulative across the whole process lifetime; no reset.

#include "common/metadata_types.h"

#ifdef MD_OP_LATENCY_PROFILE

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <string>

namespace dfs {

enum MDOpStep : int {
  kMDOpUnlink = 0,
  kMDOpRemoveDir,
  kMDOpAccess,
  kMDOpMakeDir,
  kMDOpStat,
  kMDOpRename,
  kMDOpLink,
  kMDOpCreate,
  kMDOpStepCount,
};

inline auto MDOpStepName(int s) -> const char * {
  static constexpr const char *kNames[] = {
      "Unlink", "RemoveDir", "Access", "MakeDir",
      "Stat",   "Rename",    "Link",   "Create",
  };
  return (s >= 0 && s < kMDOpStepCount) ? kNames[s] : "?";
}

constexpr int kMDOpHistBuckets = 64; // bucket k = [2^k, 2^(k+1)) ns

struct MDOpStepStats {
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> total_ns{0};
  std::atomic<uint64_t> max_ns{0};
  std::array<std::atomic<uint64_t>, kMDOpHistBuckets> hist{};
};

class MDOpProfiler {
public:
  static auto Instance() -> MDOpProfiler & {
    static MDOpProfiler inst;
    return inst;
  }

  void Record(int step, uint64_t ns) {
    if (step < 0 || step >= kMDOpStepCount) {
      return;
    }
    auto &s = stats_[step];
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.total_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t cur = s.max_ns.load(std::memory_order_relaxed);
    while (ns > cur && !s.max_ns.compare_exchange_weak(
                           cur, ns, std::memory_order_relaxed)) {
    }
    int bucket = (ns == 0) ? 0 : (63 - __builtin_clzll(ns));
    if (bucket >= kMDOpHistBuckets) {
      bucket = kMDOpHistBuckets - 1;
    }
    s.hist[bucket].fetch_add(1, std::memory_order_relaxed);
  }

  // Walk buckets low → high; return midpoint of the bucket where the
  // cumulative count first reaches `target`. Bucket k midpoint = 1.5 * 2^k.
  // Returns 0 if total==0.
  static auto PercentileNs(const MDOpStepStats &s, uint64_t total,
                           double p) -> uint64_t {
    if (total == 0) {
      return 0;
    }
    uint64_t target = static_cast<uint64_t>(
        static_cast<double>(total) * p + 0.5);
    if (target == 0) {
      target = 1;
    }
    if (target > total) {
      target = total;
    }
    uint64_t cum = 0;
    for (int k = 0; k < kMDOpHistBuckets; ++k) {
      cum += s.hist[k].load(std::memory_order_relaxed);
      if (cum >= target) {
        // Midpoint of [2^k, 2^(k+1)) = 3 * 2^(k-1) for k>=1; for k=0 use 1.
        if (k == 0) {
          return 1;
        }
        return (3ULL << (k - 1));
      }
    }
    return 0;
  }

  void Dump(const char *tag) {
    SPDLOG_INFO("── md-op profile ({}) ──", tag);
    SPDLOG_INFO("  {:<10}  {:>10}  {:>10}  {:>10}  {:>10}  {:>10}  {:>10}  "
                "{:>12}",
                "op", "count", "avg_us", "p50_us", "p99_us", "p999_us",
                "p9999_us", "max_us");
    bool any = false;
    for (int i = 0; i < kMDOpStepCount; ++i) {
      uint64_t cnt = stats_[i].count.load(std::memory_order_relaxed);
      if (cnt == 0) {
        continue;
      }
      any = true;
      uint64_t tot = stats_[i].total_ns.load(std::memory_order_relaxed);
      uint64_t mx = stats_[i].max_ns.load(std::memory_order_relaxed);
      double avg_us = static_cast<double>(tot) /
                      static_cast<double>(cnt) / 1000.0;
      double p50 = PercentileNs(stats_[i], cnt, 0.50) / 1000.0;
      double p99 = PercentileNs(stats_[i], cnt, 0.99) / 1000.0;
      double p999 = PercentileNs(stats_[i], cnt, 0.999) / 1000.0;
      double p9999 = PercentileNs(stats_[i], cnt, 0.9999) / 1000.0;
      double max_us = static_cast<double>(mx) / 1000.0;
      SPDLOG_INFO("  {:<10}  {:>10}  {:>10.2f}  {:>10.2f}  {:>10.2f}  "
                  "{:>10.2f}  {:>10.2f}  {:>12.2f}",
                  MDOpStepName(i), cnt, avg_us, p50, p99, p999, p9999, max_us);
    }
    if (!any) {
      SPDLOG_INFO("  (no samples)");
    }
    // Machine-readable raw lines for cross-log aggregation. One per op,
    // listing 64 log2 bucket counts so percentiles can be recomputed
    // accurately after summing.
    for (int i = 0; i < kMDOpStepCount; ++i) {
      uint64_t cnt = stats_[i].count.load(std::memory_order_relaxed);
      if (cnt == 0) {
        continue;
      }
      uint64_t tot = stats_[i].total_ns.load(std::memory_order_relaxed);
      uint64_t mx = stats_[i].max_ns.load(std::memory_order_relaxed);
      std::string hist;
      hist.reserve(kMDOpHistBuckets * 4);
      for (int k = 0; k < kMDOpHistBuckets; ++k) {
        if (k != 0) {
          hist.push_back(',');
        }
        hist += std::to_string(
            stats_[i].hist[k].load(std::memory_order_relaxed));
      }
      SPDLOG_INFO("  md-op-raw {} count={} total_ns={} max_ns={} hist={}",
                  MDOpStepName(i), cnt, tot, mx, hist);
    }
  }

private:
  MDOpProfiler() = default;
  std::array<MDOpStepStats, kMDOpStepCount> stats_;
};

class MDOpScopedTimer {
public:
  explicit MDOpScopedTimer(int step)
      : step_(step), start_(std::chrono::steady_clock::now()) {}
  ~MDOpScopedTimer() {
    auto end = std::chrono::steady_clock::now();
    uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_)
            .count());
    MDOpProfiler::Instance().Record(step_, ns);
  }
  MDOpScopedTimer(const MDOpScopedTimer &) = delete;
  auto operator=(const MDOpScopedTimer &) -> MDOpScopedTimer & = delete;

private:
  int step_;
  std::chrono::steady_clock::time_point start_;
};

} // namespace dfs

#define MD_OP_PROFILE_CONCAT_(a, b) a##b
#define MD_OP_PROFILE_CONCAT(a, b) MD_OP_PROFILE_CONCAT_(a, b)

#define MD_OP_PROFILE_SCOPE(step_enum)                                         \
  ::dfs::MDOpScopedTimer MD_OP_PROFILE_CONCAT(_md_op_prof_timer_,              \
                                              __COUNTER__)((step_enum))

#define MD_OP_PROFILE_DUMP(tag) ::dfs::MDOpProfiler::Instance().Dump(tag)

#else // !MD_OP_LATENCY_PROFILE

#define MD_OP_PROFILE_SCOPE(step_enum) ((void)0)
#define MD_OP_PROFILE_DUMP(tag) ((void)0)

#endif // MD_OP_LATENCY_PROFILE
