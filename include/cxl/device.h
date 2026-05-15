#pragma once

#include "common/metadata_types.h"
#include "cxl/cxl_mem.h"
#include "cxl/cxl_ssd.h"
#include "cxl/gim_context.h" // GIMGlobalHeader, DFSHeader
#include "cxl/gim_mem.h"
#include <string>

namespace dfs {

// --- CXL shared memory layout ---
// All headers at fixed offsets within first 1MB:
//   0x000 - 0xFFF  : reserved
//   0x1000          : CXLMemHeader
//   0x1000 + align  : GIMGlobalHeader (shared by all metas)
//   next align      : CXLSSDDramHeader
//   next align      : DFSHeader (hashtable offsets, per-meta allocators)
//   0x100000 (1MB)  : start of CXL memory pool

struct CXLDeviceLayout {
  static constexpr uint64_t kHeaderBase = 4096;              // 4KB
  static constexpr uint64_t kPoolStart = 1ULL * 1024 * 1024; // 1MB

  static constexpr uint64_t kCXLMemHeaderOffset = kHeaderBase;

  static auto GIMHeaderOffset() -> uint64_t {
    return kCXLMemHeaderOffset + ((sizeof(CXLMemHeader) + 63) / 64) * 64;
  }

  static auto SSDHeaderOffset() -> uint64_t {
    return GIMHeaderOffset() + ((sizeof(GIMGlobalHeader) + 63) / 64) * 64;
  }

  static auto DFSHeaderOffset() -> uint64_t {
    return SSDHeaderOffset() + ((sizeof(CXLSSDDramHeader) + 63) / 64) * 64;
  }
};

constexpr static int kMaxMetaNumOfGroup = 8;

// DFS shared header — stored in CXL header area (after CXLSSDDramHeader).
// Contains per-meta allocators/NUMA info and shared hashtable offsets.
struct DFSHeader {
  int numa_node_[kMaxMetaNumOfGroup];
  NaiveAllocator allocator_[kMaxMetaNumOfGroup];

  // CXL offset of shared ResizeMeta (allocated by meta 0, used by all), Declare
  // in include/server/level_hashtable.h
  // struct alignas(64) ResizeMeta {
  //   uint64_t ctrl = 0;
  //   uint64_t version = 0;
  //   uint64_t tl_offset = 0;
  //   uint64_t tl_size = 0;
  //   uint64_t bl_offset = 0;
  //   uint64_t bl_size = 0;
  //   uint64_t old_bl_offset = 0;
  //   uint64_t old_bl_size = 0;
  // };
  std::atomic<uint64_t> magic_num_{0};
  uint64_t d_resize_meta_offset_ = UINT64_MAX;
  uint64_t i_resize_meta_offset_ = UINT64_MAX;

  // GIM offset of per-MDS resize notification area (dirent / inode)
  // Each MDS allocates GIMResizeNotify in its own GIM local memory,
  // stores offset here so other MDS can GIMWriteSync to it.
  uint64_t d_resize_notify_offset_[kMaxMetaNumOfGroup];
  uint64_t i_resize_notify_offset_[kMaxMetaNumOfGroup];

  // Per-MDS InodeArrayBlock CXL offset.
  // Each meta writes its own slot at init; other metas read via SetMds().
  uint64_t inode_array_block_offset_[kMaxMetaNumOfGroup];

  inline void Init() {
    for (auto &o : d_resize_notify_offset_) {
      o = UINT64_MAX;
    }
    for (auto &o : i_resize_notify_offset_) {
      o = UINT64_MAX;
    }
    for (auto &o : inode_array_block_offset_) {
      o = 0;
    }
  }

  auto IsInit() -> bool {
    return magic_num_.load(std::memory_order_acquire) == 0x43584C53;
  }
};

// --- CXLDevice: unified container for CXL subsystem ---

struct CXLDevice {
  CXLMem *cxl = nullptr;
  CXLSSD *ssd = nullptr;
  GIMMem *gim = nullptr;

  // Shared headers in CXL memory
  // GIMGlobalHeader *gim_header_ = nullptr;
  DFSHeader *dfs_header_ = nullptr;

  ~CXLDevice();

  auto Init(int meta_num, uint64_t cxl_capacity_GiB = CXL_CAPACITY,
            const std::string &cxl_path = "/dev/hugepages/cxl_memory",
            int cxl_numa_node = CXL_NUMA_NODE,
            uint64_t gim_per_meta_MiB = GIM_CAPACITY,
            const std::string &gim_path = "/dev/hugepages/gim",
            int gim_numa_node = 1, uint64_t ssd_dram_MiB = CXLSSD_CAPACITY_MB,
            const std::string &flash_path = "") -> int;

  // ===== CXL memory operations =====

  inline auto CXLReadSync(uint64_t offset, uint64_t size, void *data) -> int {
    return cxl->Read(offset, size, data);
  }

  inline auto CXLWriteSync(uint64_t offset, uint64_t size, void *data) -> int {
    return cxl->Write(offset, size, data);
  }

  inline auto CXLAtomicCasSync(uint64_t offset, uint64_t compare_val,
                               uint64_t swap_val, uint64_t *local_buf) -> int {
    return cxl->Cas(offset, compare_val, swap_val, local_buf);
  }

  // CXL memory allocation
  inline auto CXLMemMalloc(uint64_t size) -> uint64_t {
    return cxl->Alloc(size);
  }

  inline auto CXLMemMallocPointer(uint64_t size) -> void * {
    uint64_t offset = cxl->Alloc(size);
    return cxl->BufPtr() + offset;
  }

  inline void CXLMemFree(uint64_t offset, uint64_t size) {
    cxl->Free(offset, size);
  }

  inline void CXLMemFreePointer(void *ptr, uint64_t size) {
    cxl->Allocator()->CXLFakeFree(ptr, size);
  }

  inline void CXLMemReset(uint64_t offset, uint64_t size) {
    cxl->Reset(offset, size);
  }

  // ===== GIM operations =====

  // GIM local allocation
  inline auto GIMMemMallocPointer(uint64_t size) -> void * {
    return gim->AllocLocal(size);
  }

  inline void GIMMemFreePointer(void *ptr, uint64_t size) {
    gim->FreeLocal(ptr, size);
  }

  inline auto GetGIMMemOffset(void *ptr) -> uint64_t {
    return gim->GetOffset(ptr);
  }

  // GIM remote operations (new: with target_meta)
  inline auto GIMReadSync(int target_meta, uint64_t offset, uint64_t size,
                          void *data) -> int {
    return gim->RemoteRead(target_meta, offset, size, data);
  }

  inline auto GIMWriteSync(int target_meta, uint64_t offset, uint64_t size,
                           void *data) -> int {
    return gim->RemoteWrite(target_meta, offset, size, data);
  }

  inline auto GIMAtomicCasSync(int target_meta, uint64_t offset,
                               uint64_t compare_val, uint64_t swap_val,
                               uint64_t *local_buf) -> int {
    return gim->RemoteCas(target_meta, offset, compare_val, swap_val,
                          local_buf);
  }

  inline auto GIMAtomicFAASync(int target_meta, uint64_t offset,
                               uint64_t add_val, uint64_t *local_buf) -> int {
    return gim->RemoteFAA(target_meta, offset, add_val, local_buf);
  }

  // ===== CXL SSD operations =====
  inline auto SSDDramAlloc(uint64_t size, uint64_t align = 8) -> uint64_t {
    return ssd->DramAlloc(size, align);
  }

  inline auto SSDDramRead(uint64_t offset, uint64_t size, void *buf) -> int {
    return ssd->DramRead(offset, size, buf);
  }

  inline auto SSDDramWrite(uint64_t offset, uint64_t size, void *buf) -> int {
    return ssd->DramWrite(offset, size, buf);
  }

  inline auto SSDDramBase() -> void * { return ssd->DramBase(); }

  inline auto SSDDramCapacity() const -> uint64_t {
    return ssd->DramCapacity();
  }
};

// Global device instance
extern CXLDevice *gDevice;

#define gDualMem gDevice
using DualMemDevice = CXLDevice;

void InitDevice(int meta_num, uint64_t cxl_capacity_GiB = CXL_CAPACITY,
                const std::string &cxl_path = "/dev/hugepages/cxl_memory",
                int cxl_numa_node = CXL_NUMA_NODE,
                uint64_t gim_per_meta_MiB = GIM_CAPACITY,
                const std::string &gim_path = "/dev/hugepages/gim",
                int gim_numa_node = 1,
                uint64_t ssd_dram_MiB = CXLSSD_CAPACITY_MB,
                const std::string &flash_path = "");

void DestroyDevice();

} // namespace dfs
