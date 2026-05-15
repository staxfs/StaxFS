#pragma once

#include "cxl/cxl_mem.h"
#include "spdlog/spdlog.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace dfs {

// CXL SSD DRAM header (stored in CXL shared memory)
struct CXLSSDDramHeader {
  std::atomic<uint64_t> magic_num_{0};
  std::atomic<uint64_t> alloc_pos_; // bump allocator within DRAM region
  uint64_t capacity_;               // usable DRAM capacity
  uint64_t base_offset_;            // base offset in CXLMem pool

  auto IsInit() -> bool {
    return magic_num_.load(std::memory_order_acquire) == 0x43584C53;
  } // "CXLS"

  void Init(uint64_t base_offset, uint64_t capacity) {
    alloc_pos_.store(0, std::memory_order_relaxed);
    base_offset_ = base_offset;
    capacity_ = capacity;
    magic_num_.store(0x43584C53, std::memory_order_release);
  }
};

// --- CXLSSD: CXL SSD emulator ---
// DRAM cache: allocated from CXLMem pool (same delay as CXL extended memory).
// Flash backend: pread/pwrite to NVMe SSD (no extra delay).
//
// Flash files are split into two per-MDS backing files so inode and dirent
// persistence are physically separated:
//   <flash_path>/Meta<N>_Inode   — SSDInodeRegion backing file
//   <flash_path>/Meta<N>_Dent    — SSDDentRegion  backing file
// Each region writes at offset 0 within its own file.

class CXLSSD {
public:
  CXLSSD() = default;
  ~CXLSSD();

  // Initialize:
  //   - Allocate DRAM region from cxl_mem (shares CXL memory pool + delay)
  //   - Open two flash backing files under flash_path (inode / dent)
  //   - header is stored in CXL shared memory
  auto Init(CXLMem *cxl_mem, uint64_t dram_capacity_MiB,
            const std::string &flash_path, CXLSSDDramHeader *header,
            int meta_num) -> int;

  // --- DRAM operations (delegates to CXLMem, same delay as CXL extended mem)
  // ---

  // Allocate from DRAM region (thread-safe bump allocator)
  inline auto DramAlloc(uint64_t size, uint64_t align = 64) -> uint64_t {
    size = (size + align - 1) & ~(align - 1);
    uint64_t pos =
        header_->alloc_pos_.fetch_add(size, std::memory_order_acq_rel);
    if (pos + size > header_->capacity_) {
      SPDLOG_ERROR("CXLSSD::DramAlloc: OOM, requested {} at pos {}, cap {}",
                   size, pos, header_->capacity_);
      return UINT64_MAX;
    }
    return pos; // offset relative to DRAM region start
  }

  // Read from DRAM (goes through CXLMem::Read with switch delay)
  inline auto DramRead(uint64_t offset, uint64_t size, void *buf) -> int {
    return cxl_mem_->Read(header_->base_offset_ + offset, size, buf);
  }

  // Write to DRAM (goes through CXLMem::Write with switch delay)
  inline auto DramWrite(uint64_t offset, uint64_t size, void *buf) -> int {
    return cxl_mem_->Write(header_->base_offset_ + offset, size, buf);
  }

  // Get raw pointer to DRAM region (for placement new / direct init)
  inline auto DramBase() -> void * {
    return cxl_mem_->BufPtr() + header_->base_offset_;
  }

  inline auto DramCapacity() const -> uint64_t { return header_->capacity_; }

  // --- Flash operations (direct NVMe, no extra delay) ---
  //
  // The inode and dirent regions each have a dedicated backing file.
  // Offsets below are file-local (both regions start at offset 0).

  void InodeWrite(uint64_t offset, const void *data, size_t len);
  void InodeRead(uint64_t offset, void *data, size_t len);
  void InodeSync();

  void DentWrite(uint64_t offset, const void *data, size_t len);
  void DentRead(uint64_t offset, void *data, size_t len);
  void DentSync();

  auto IsInitialized() const -> bool { return initialized_; }

private:
  CXLMem *cxl_mem_ = nullptr;          // reference (not owned)
  CXLSSDDramHeader *header_ = nullptr; // in CXL shared memory
  int flash_fd_inode_ = -1;
  int flash_fd_dent_ = -1;
  bool initialized_ = false;
};

} // namespace dfs
