#pragma once

#include "cxl/cxl_allocator.h"
#include "cxl/cxl_mem.h" // for INSERT_DELAY macro
#include "spdlog/spdlog.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <string>
#include <x86intrin.h>

// GIM delay iteration counts for RDMA latency simulation.
// GIM uses UIO (CXL.io) with RDMA semantics through CXL Fabric (CXL 4.0
// §7.7.3). Measured cross-machine RDMA latency (see RDMA_latency.log):
//   RDMA Write: 1.27us, RDMA Read: 2.60us, RDMA CAS: 2.61us
//
// INSERT_DELAY(N) = 2×mfence(~54ns) + N×(rdtsc+pause)(~31ns) at 2.4GHz.
// GIM memory is on LOCAL DRAM, so Remote function body cost = DRAM access.
// Calibrated on Xeon Gold 6448H, Node1 DRAM:
//   RemoteRead  body: ~584ns → delay(63) ~2007ns → total ~2591ns (target
//   2600ns) RemoteWrite body: ~358ns → delay(28)  ~922ns → total ~1280ns
//   (target 1270ns) RemoteCas   body:  ~99ns → delay(79) ~2503ns → total
//   ~2602ns (target 2610ns) Local access: zero delay (~6ns read, ~6ns write,
//   ~14ns CAS)
#define GIM_READ_DELAY_ITERS 63
#define GIM_WRITE_DELAY_ITERS 28
#define GIM_ATOMIC_DELAY_ITERS 79

namespace dfs {

constexpr int kMaxMetaNum = 16;

// --- GIMMem: per-meta local DRAM with Local/Remote access paths ---
//
// Each meta server has its own GIM region on local DRAM.
// - Local access (own memory): zero delay, normal memcpy/load/store.
// - Remote access (other meta's memory): inject RDMA delay.
//
// Memory layout:
//   GIM_BASE + 0 * SIZE → Meta 0's hugepage (local DRAM)
//   GIM_BASE + 1 * SIZE → Meta 1's hugepage (local DRAM)
//   ...
// All metas mmap all regions at fixed VAs after init.

class GIMMem {
public:
  GIMMem() = default;
  ~GIMMem();

  // Initialize: mmap own hugepage on local DRAM.
  // Allocator and meta count are managed externally by CXLDevice.
  auto Init(int meta_num, uint64_t cxl_capacity, uint64_t per_meta_MiB,
            const std::string &hugepage_base_path, int numa_node) -> int;

  // Map all other metas' GIM regions into this process's VA space.
  // numa_nodes: array of NUMA node IDs for each meta (for mbind).
  // If nullptr, mbind is skipped (default NUMA policy).
  void MapOtherMetas(const int *numa_nodes = nullptr);

  // --- Allocation (from own local region) ---
  inline auto AllocLocal(uint64_t size) -> void * {
    uint64_t offset = my_alloc_->CXLMallocOffset(size);
    return my_buf_ + offset;
  }

  inline void FreeLocal(void *ptr, uint64_t size) {
    my_alloc_->CXLFakeFree(ptr, size);
  }

  inline auto GetOffset(void *ptr) -> uint64_t {
    return static_cast<uint64_t>(static_cast<uint8_t *>(ptr) - my_buf_);
  }

  // --- Local access (own memory, ZERO extra delay) ---

  inline void LocalRead(uint64_t offset, uint64_t size, void *buf) {
    ::memcpy(buf, my_buf_ + offset, size);
  }

  inline void LocalWrite(uint64_t offset, uint64_t size, const void *buf) {
    ::memcpy(my_buf_ + offset, buf, size);
  }

  inline auto LocalCas(uint64_t offset, uint64_t compare_val, uint64_t swap_val,
                       uint64_t *old_val) -> int {
    auto *addr = reinterpret_cast<uint64_t *>(my_buf_ + offset);
    *old_val = __sync_val_compare_and_swap(addr, compare_val, swap_val);
    return (*old_val == compare_val) ? 0 : -1;
  }

  inline auto LocalFAA(uint64_t offset, uint64_t add_val,
                       uint64_t *old_val) -> int {
    auto *addr = reinterpret_cast<uint64_t *>(my_buf_ + offset);
    *old_val = __sync_fetch_and_add(addr, add_val);
    return 0;
  }

  // --- Remote access (other meta's memory, with RDMA delay) ---

  inline auto RemoteRead(int target_meta, uint64_t offset, uint64_t size,
                         void *data) -> int {
    if (target_meta < 0 || target_meta >= total_metas_ ||
        !meta_bufs_[target_meta]) {
      SPDLOG_ERROR("GIMMem::RemoteRead: invalid target_meta {}", target_meta);
      return -1;
    }

    INSERT_DELAY(GIM_READ_DELAY_ITERS);
    char *src = reinterpret_cast<char *>(meta_bufs_[target_meta]) + offset;
    char *dst = static_cast<char *>(data);

    // clflushopt + SSE load (simulate RDMA DMA bypassing remote cache)
    for (size_t i = 0; i < size; i += 64) {
      _mm_clflushopt(src + i);
    }
    _mm_mfence();

    for (size_t i = 0; i < size; i += 64) {
      __m128i d0 = _mm_load_si128(reinterpret_cast<const __m128i *>(src + i));
      __m128i d1 =
          _mm_load_si128(reinterpret_cast<const __m128i *>(src + i + 16));
      __m128i d2 =
          _mm_load_si128(reinterpret_cast<const __m128i *>(src + i + 32));
      __m128i d3 =
          _mm_load_si128(reinterpret_cast<const __m128i *>(src + i + 48));
      _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i), d0);
      _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i + 16), d1);
      _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i + 32), d2);
      _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i + 48), d3);
    }
    _mm_sfence();
    return 0;
  }

  inline auto RemoteWrite(int target_meta, uint64_t offset, uint64_t size,
                          void *data) -> int {
    if (target_meta < 0 || target_meta >= total_metas_ ||
        !meta_bufs_[target_meta]) {
      SPDLOG_ERROR("GIMMem::RemoteWrite: invalid target_meta {}", target_meta);
      return -1;
    }

    INSERT_DELAY(GIM_WRITE_DELAY_ITERS);
    char *dst = reinterpret_cast<char *>(meta_bufs_[target_meta]) + offset;
    char *src = static_cast<char *>(data);

    for (ssize_t i = size - 64; i >= 0; i -= 64) {
      __m128i d0 = _mm_load_si128(reinterpret_cast<const __m128i *>(src + i));
      __m128i d1 =
          _mm_load_si128(reinterpret_cast<const __m128i *>(src + i + 16));
      __m128i d2 =
          _mm_load_si128(reinterpret_cast<const __m128i *>(src + i + 32));
      __m128i d3 =
          _mm_load_si128(reinterpret_cast<const __m128i *>(src + i + 48));
      _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i + 48), d3);
      _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i + 32), d2);
      _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i + 16), d1);
      _mm_stream_si128(reinterpret_cast<__m128i *>(dst + i), d0);
      _mm_clwb(dst + i);
    }
    _mm_sfence();
    return 0;
  }

  inline auto RemoteCas(int target_meta, uint64_t offset, uint64_t compare_val,
                        uint64_t swap_val, uint64_t *old_val) -> int {
    if (target_meta < 0 || target_meta >= total_metas_ ||
        !meta_bufs_[target_meta]) {
      SPDLOG_ERROR("GIMMem::RemoteCas: invalid target_meta {}", target_meta);
      return -1;
    }

    INSERT_DELAY(GIM_ATOMIC_DELAY_ITERS);
    auto *addr = reinterpret_cast<uint64_t *>(meta_bufs_[target_meta] + offset);
    *old_val = __sync_val_compare_and_swap(addr, compare_val, swap_val);
    return (*old_val == compare_val) ? 0 : -1;
  }

  inline auto RemoteFAA(int target_meta, uint64_t offset, uint64_t add_val,
                        uint64_t *old_val) -> int {
    if (target_meta < 0 || target_meta >= total_metas_ ||
        !meta_bufs_[target_meta]) {
      SPDLOG_ERROR("GIMMem::RemoteFAA: invalid target_meta {}", target_meta);
      return -1;
    }

    INSERT_DELAY(GIM_ATOMIC_DELAY_ITERS);
    auto *addr = reinterpret_cast<uint64_t *>(meta_bufs_[target_meta] + offset);
    *old_val = __sync_fetch_and_add(addr, add_val);
    return 0;
  }

  // --- Accessors ---
  auto MyMetaId() const -> int { return my_meta_id_; }

  auto TotalMetas() const -> int { return total_metas_; }

  auto MyBuf() const -> uint8_t * { return my_buf_; }

  auto MetaBuf(int meta_id) const -> uint8_t * { return meta_bufs_[meta_id]; }

  auto MyAllocator() -> NaiveAllocator * { return my_alloc_; }

  void SetAllocator(NaiveAllocator *alloc) { my_alloc_ = alloc; }

  void SetTotalMetas(int n) { total_metas_ = n; }

  // Backward compatibility aliases
  inline auto GIMMemMallocPointer(uint64_t size) -> void * {
    return AllocLocal(size);
  }

  inline void GIMMemFreePointer(void *ptr, uint64_t size) {
    FreeLocal(ptr, size);
  }

  inline auto GetGIMMemOffset(void *ptr) -> uint64_t { return GetOffset(ptr); }

  // For test: set up multiple metas with heap-backed buffers (no hugepages).
  // bufs[i] is the buffer for meta i, alloc is for meta my_meta.
  void SetTestBacking(int my_meta, int total_metas, uint8_t *bufs[],
                      NaiveAllocator *alloc) {
    my_meta_id_ = my_meta;
    total_metas_ = total_metas;
    my_buf_ = bufs[my_meta];
    my_alloc_ = alloc;
    for (int i = 0; i < total_metas; i++) {
      meta_bufs_[i] = bufs[i];
    }
  }

private:
  int my_meta_id_ = -1;
  int total_metas_ = 0;
  uint64_t per_meta_size_ = 0;
  uint64_t cxl_capacity_ = 0;
  std::string hugepage_base_path_;

  uint8_t *my_buf_ = nullptr;
  uint8_t *meta_bufs_[kMaxMetaNum] = {};
  NaiveAllocator *my_alloc_ = nullptr;
};

} // namespace dfs
