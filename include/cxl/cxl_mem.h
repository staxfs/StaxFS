#pragma once

#include "cxl/cxl_allocator.h"
#include "cxl/cxl_base.h"
#include "spdlog/spdlog.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <string>
#include <x86intrin.h>

// --- Delay injection ---

#define INSERT_DELAY(times)                                                    \
  _mm_mfence();                                                                \
  for (int i = 0; i < (times); i++) {                                          \
    _rdtsc();                                                                  \
    __asm__ volatile("pause" ::: "memory");                                    \
  }                                                                            \
  _mm_mfence();

// CXL delay iteration counts for switch latency simulation.
// INSERT_DELAY(N) = 2×mfence(~54ns) + N×(rdtsc+pause)(~31ns) at 2.4GHz.
//
// Target: CXL Read/Write = 600ns, CAS (mCAS via FPGA NMP) = 2300ns.
//
// Calibrated on Xeon Gold 6448H, Node1 CPU → Node4 CXL:
//   Read(64B):  body ~481ns + delay(2)  ~116ns = ~597ns  (target 600ns)
//   Write(64B): body ~381ns + delay(5)  ~209ns = ~590ns  (target 600ns)
//   CAS:        body  ~16ns + delay(72) ~2286ns = ~2302ns (target 2300ns)
#define CXL_READ_DELAY_ITERS 2
#define CXL_WRITE_DELAY_ITERS 5
#define CXL_CAS_DELAY_ITERS 72

namespace dfs {

// --- CXL shared memory header (lives in CXL memory, shared by all metas) ---

struct CXLMemHeader {
  std::atomic<uint64_t> magic_num_{0};
  std::atomic<uint64_t> meta_finish_{0};
  NaiveAllocator allocator_;

  auto IsInit() -> bool {
    return magic_num_.load(std::memory_order_acquire) == 0x43584C53;
  }

  void Init(void *buf, uint64_t capacity) {
    if (magic_num_ != 0x8767987a) {
      SPDLOG_INFO("Init CXLMemHeader");
      allocator_.Init(buf, capacity);
      magic_num_.store(0x43584C53, std::memory_order_release);
    }
  }
};

// --- CXLMem: CXL extended memory module ---
// Handles: mmap hugepages, allocation, read/write/cas with delay injection.
// Memory is on CXL NUMA node (Node 4), shared by all meta servers.

class CXLMem {
public:
  CXLMem() = default;
  ~CXLMem();

  // Initialize: mmap hugepages on numa_node, setup allocator.
  // meta_num==0 resets the region; others wait for init.
  auto Init(int meta_num, uint64_t capacity_GiB, const std::string &path,
            int numa_node) -> int;

  // --- Allocation ---
  // Returns offset relative to buf_ptr_ (consistent with Read/Write/Cas).
  inline auto Alloc(uint64_t size) -> uint64_t {
    return allocator_->CXLMallocOffset(size) + pool_base_;
  }

  inline void Free(uint64_t offset, uint64_t size) {
    allocator_->CXLFakeFree(buf_ptr_ + offset, size);
  }

  inline void Reset(uint64_t offset, uint64_t size) {
    ::memset(buf_ptr_ + offset, 0, size);
  }

  // --- Accessors ---
  auto BufPtr() const -> uint8_t * { return buf_ptr_; }

  auto Capacity() const -> uint64_t { return capacity_; }

  auto Header() -> CXLMemHeader * { return header_; }

  auto Allocator() -> NaiveAllocator * { return allocator_; }

  // --- Read from CXL memory to local buffer (with switch delay) ---
  // clflushopt invalidates cache → forces re-read from CXL device.
  inline auto Read(uint64_t offset, uint64_t size, void *data) -> int {
    if (offset + size > pool_base_ + allocator_->offset_) {
      SPDLOG_ERROR("CXLMem::Read: offset({}) + size({}) > pool end({})", offset,
                   size, pool_base_ + allocator_->offset_.load());
      return -1;
    }

    INSERT_DELAY(CXL_READ_DELAY_ITERS);
    char *src = reinterpret_cast<char *>(buf_ptr_ + offset);
    char *dst = static_cast<char *>(data);

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

  // --- Write from local buffer to CXL memory (with switch delay) ---
  // Reverse order for crash consistency (header at low addr written last).
  inline auto Write(uint64_t offset, uint64_t size, void *data) -> int {
    if (offset + size > pool_base_ + allocator_->offset_) {
      SPDLOG_ERROR("CXLMem::Write: offset({}) + size({}) > pool end({})",
                   offset, size, pool_base_ + allocator_->offset_.load());
      return -1;
    }

    INSERT_DELAY(CXL_WRITE_DELAY_ITERS);
    char *dst = reinterpret_cast<char *>(buf_ptr_ + offset);
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

  // --- CAS on CXL memory (simulates mCAS via FPGA NMP) ---
  inline auto Cas(uint64_t offset, uint64_t compare_val, uint64_t swap_val,
                  uint64_t *local_buf) -> int {
    if ((offset & 7) != 0) {
      SPDLOG_CRITICAL("CXLMem::Cas: offset must be 8-byte aligned");
    }
    if (offset + 8 > pool_base_ + allocator_->offset_) {
      SPDLOG_ERROR("CXLMem::Cas: offset({}) + 8 > pool end({})", offset,
                   pool_base_ + allocator_->offset_.load());
      return -1;
    }

    INSERT_DELAY(CXL_CAS_DELAY_ITERS);
    auto *remote_addr = reinterpret_cast<uint64_t *>(buf_ptr_ + offset);
    uint64_t old_val =
        __sync_val_compare_and_swap(remote_addr, compare_val, swap_val);
    *local_buf = old_val;
    return (old_val == compare_val) ? 0 : -1;
  }

  // --- Backward compatibility aliases ---
  inline auto CXLReadSync(uint64_t offset, uint64_t size, void *data) -> int {
    return Read(offset, size, data);
  }

  inline auto CXLWriteSync(uint64_t offset, uint64_t size, void *data) -> int {
    return Write(offset, size, data);
  }

  inline auto CXLAtomicCasSync(uint64_t offset, uint64_t compare_val,
                               uint64_t swap_val, uint64_t *local_buf) -> int {
    return Cas(offset, compare_val, swap_val, local_buf);
  }

  inline auto CXLMemMalloc(uint64_t size) -> uint64_t { return Alloc(size); }

  inline void CXLMemFree(uint64_t offset, uint64_t size) { Free(offset, size); }

  inline void CXLMemReset(uint64_t offset, uint64_t size) {
    Reset(offset, size);
  }

  // Called by CXLDevice after mmap to attach the shared header/allocator.
  void AttachHeader(CXLMemHeader *header) {
    header_ = header;
    allocator_ = &header->allocator_;
    pool_base_ = static_cast<uint64_t>(allocator_->buf_ - buf_ptr_);
  }

  // For test: set buf/capacity directly (heap-backed, no mmap)
  void SetTestBacking(uint8_t *buf, uint64_t capacity, NaiveAllocator *alloc) {
    buf_ptr_ = buf;
    capacity_ = capacity;
    allocator_ = alloc;
    pool_base_ = static_cast<uint64_t>(alloc->buf_ - buf);
    owns_mmap_ = false;
  }

private:
  uint8_t *buf_ptr_ = nullptr;
  uint64_t capacity_ = 0;
  CXLMemHeader *header_ = nullptr;
  NaiveAllocator *allocator_ = nullptr;
  uint64_t pool_base_ = 0; // offset from buf_ptr_ to allocator_->buf_
  bool owns_mmap_ = false; // true if we mmap'd (vs test heap-backed)
};

} // namespace dfs
