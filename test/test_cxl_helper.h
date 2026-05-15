#pragma once

// Lightweight CXL test helper: creates a heap-backed CXLDevice
// so unit tests don't require hugetlbfs, NUMA, or real CXL hardware.
//
// Usage:
//   dfs::test::TestCXLSetup setup(128 * 1024 * 1024);  // 128MB
//   // gDevice (gDualMem) is now valid — create hash tables, etc.
//   setup.reset();  // optional: wipe memory and reset allocator

#include "cxl/cxl_allocator.h"
#include "cxl/device.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace dfs {
namespace test {

class TestCXLSetup {
public:
  explicit TestCXLSetup(size_t capacity_bytes = 128 * 1024 * 1024,
                        uint64_t ssd_dram_mib = 64) {
    capacity_ = capacity_bytes;
    ssd_dram_mib_ = ssd_dram_mib;

    // Allocate 64-byte aligned heap memory
    int ret =
        posix_memalign(reinterpret_cast<void **>(&buf_), 64, capacity_bytes);
    assert(ret == 0 && buf_);
    std::memset(buf_, 0, capacity_bytes);

    // Build a NaiveAllocator on the heap
    alloc_ = new NaiveAllocator();
    auto *pool_start = buf_ + CXLDeviceLayout::kPoolStart;
    alloc_->Init(pool_start, capacity_bytes - CXLDeviceLayout::kPoolStart);

    // Build CXLMem with heap backing (no hugepages)
    cxl_mem_ = new CXLMem();
    cxl_mem_->SetTestBacking(buf_, capacity_bytes, alloc_);

    // Initialize shared headers expected by Metadata / CXL persistence.
    auto *cxl_header = reinterpret_cast<CXLMemHeader *>(
        buf_ + CXLDeviceLayout::kCXLMemHeaderOffset);
    auto *gim_header = reinterpret_cast<GIMGlobalHeader *>(
        buf_ + CXLDeviceLayout::GIMHeaderOffset());
    auto *ssd_header = reinterpret_cast<CXLSSDDramHeader *>(
        buf_ + CXLDeviceLayout::SSDHeaderOffset());
    dfs_header_ = reinterpret_cast<DFSHeader *>(
        buf_ + CXLDeviceLayout::DFSHeaderOffset());
    cxl_header->Init(pool_start, capacity_bytes - CXLDeviceLayout::kPoolStart);
    gim_header->Init();
    dfs_header_->Init();
    cxl_mem_->AttachHeader(cxl_header);

    // Build a heap-backed single-meta GIM region.
    int ret_gim = posix_memalign(reinterpret_cast<void **>(&gim_buf_), 64,
                                 kGimCapacityBytes);
    assert(ret_gim == 0 && gim_buf_);
    std::memset(gim_buf_, 0, kGimCapacityBytes);
    gim_alloc_ = new NaiveAllocator();
    gim_alloc_->Init(gim_buf_, kGimCapacityBytes);

    gim_ = new GIMMem();
    uint8_t *gim_bufs[1] = {gim_buf_};
    gim_->SetTestBacking(0, 1, gim_bufs, gim_alloc_);
    dfs_header_->allocator_[0].Init(gim_buf_, kGimCapacityBytes);
    dfs_header_->numa_node_[0] = 0;
    gim_->SetAllocator(&dfs_header_->allocator_[0]);
    gim_->SetTotalMetas(1);

    // Create a temporary flash backing directory so CXLSSD can open its
    // per-region files (Meta<N>_Inode / Meta<N>_Dent) inside it.
    char dir_template[] = "/tmp/dfs-cxlssd-flash-XXXXXX";
    const char *dir = mkdtemp(dir_template);
    assert(dir != nullptr);
    flash_path_ = dir;

    ssd_ = new CXLSSD();
    int rc = ssd_->Init(cxl_mem_, ssd_dram_mib_, flash_path_, ssd_header, 0);
    assert(rc == 0);

    // Build CXLDevice
    dev_ = new CXLDevice();
    dev_->cxl = cxl_mem_;
    dev_->gim = gim_;
    dev_->ssd = ssd_;
    dev_->dfs_header_ = dfs_header_;

    gDevice = dev_;
  }

  ~TestCXLSetup() {
    gDevice = nullptr;
    dev_->cxl = nullptr; // prevent double-free (we own cxl_mem_)
    dev_->gim = nullptr;
    dev_->ssd = nullptr;
    delete dev_;
    delete ssd_;
    delete gim_;
    delete cxl_mem_;
    delete gim_alloc_;
    delete alloc_;
    std::free(gim_buf_);
    std::free(buf_);
    if (!flash_path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(flash_path_, ec);
    }
  }

  void reset() {
    std::memset(buf_, 0, capacity_);
    auto *pool_start = buf_ + CXLDeviceLayout::kPoolStart;
    // Re-init the in-buf CXLMemHeader (wiped by the memset above) and
    // re-attach it so cxl_mem_'s active allocator_ pointer picks up the
    // fresh in-buf allocator. Without this, cxl_mem_->allocator_ still
    // points to the zeroed in-buf allocator whose capacity_=0 → every
    // subsequent CXLMemMalloc silently OOMs and returns pool_base_, and
    // every hashtable shares offset 0.
    auto *cxl_header = reinterpret_cast<CXLMemHeader *>(
        buf_ + CXLDeviceLayout::kCXLMemHeaderOffset);
    cxl_header->Init(pool_start, capacity_ - CXLDeviceLayout::kPoolStart);
    cxl_mem_->AttachHeader(cxl_header);
    std::memset(gim_buf_, 0, kGimCapacityBytes);
    gim_alloc_->Init(gim_buf_, kGimCapacityBytes);
    dfs_header_->Init();
    dfs_header_->allocator_[0].Init(gim_buf_, kGimCapacityBytes);
    dfs_header_->numa_node_[0] = 0;
    gim_->SetAllocator(&dfs_header_->allocator_[0]);
  }

  TestCXLSetup(const TestCXLSetup &) = delete;
  TestCXLSetup &operator=(const TestCXLSetup &) = delete;

private:
  static constexpr size_t kGimCapacityBytes = 4 * 1024 * 1024;
  uint8_t *buf_ = nullptr;
  uint8_t *gim_buf_ = nullptr;
  size_t capacity_ = 0;
  NaiveAllocator *alloc_ = nullptr;
  NaiveAllocator *gim_alloc_ = nullptr;
  CXLMem *cxl_mem_ = nullptr;
  GIMMem *gim_ = nullptr;
  CXLSSD *ssd_ = nullptr;
  DFSHeader *dfs_header_ = nullptr;
  CXLDevice *dev_ = nullptr;
  std::string flash_path_;
  uint64_t ssd_dram_mib_ = 0;
};

// Backward compat alias
using TestDualMemSetup = TestCXLSetup;

} // namespace test
} // namespace dfs
