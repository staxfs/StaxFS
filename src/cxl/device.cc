#include "cxl/device.h"

#include "spdlog/spdlog.h"
#include <cassert>
#include <thread>

namespace dfs {

CXLDevice *gDevice = nullptr;

auto CXLDevice::Init(int meta_num, uint64_t cxl_capacity_GiB,
                     const std::string &cxl_path, int cxl_numa_node,
                     uint64_t gim_per_meta_MiB, const std::string &gim_path,
                     int gim_numa_node, uint64_t ssd_dram_MiB,
                     const std::string &flash_path) -> int {
  uint64_t GiB = 1024ULL * 1024ULL * 1024ULL;
  uint64_t MiB = 1024ULL * 1024ULL;

  // --- 1. Initialize CXL memory ---
  cxl = new CXLMem();
  if (cxl->Init(meta_num, cxl_capacity_GiB, cxl_path, cxl_numa_node) != 0) {
    SPDLOG_ERROR("CXLDevice: CXLMem init failed");
    return -1;
  }

  // --- 2. Setup headers in CXL shared memory ---
  uint8_t *base = cxl->BufPtr();
  auto *cxl_header = reinterpret_cast<CXLMemHeader *>(
      base + CXLDeviceLayout::kCXLMemHeaderOffset);
  auto *gim_header = reinterpret_cast<GIMGlobalHeader *>(
      base + CXLDeviceLayout::GIMHeaderOffset());
  auto *ssd_header = reinterpret_cast<CXLSSDDramHeader *>(
      base + CXLDeviceLayout::SSDHeaderOffset());
  dfs_header_ =
      reinterpret_cast<DFSHeader *>(base + CXLDeviceLayout::DFSHeaderOffset());

  assert(CXLDeviceLayout::DFSHeaderOffset() + sizeof(DFSHeader) <
         CXLDeviceLayout::kPoolStart);

  // Meta 0 initializes shared headers and allocator pool
  if (meta_num == 0) {
    void *pool_start = base + CXLDeviceLayout::kPoolStart;
    uint64_t pool_size = cxl_capacity_GiB * GiB - CXLDeviceLayout::kPoolStart;
    cxl->CXLMemReset(CXLDeviceLayout::kPoolStart, pool_size);
    cxl_header->Init(pool_start, pool_size);
    gim_header->Init();
    dfs_header_->Init();
  }

  // Wait for CXL header init, then attach allocator
  while (!cxl_header->IsInit()) {
  }
  cxl->AttachHeader(cxl_header);

  while (!gim_header->IsInit()) {
  }

  SPDLOG_INFO(
      "CXLDevice: CXL header at {}, allocator pool at {}", fmt::ptr(cxl_header),
      fmt::ptr(cxl_header->allocator_.buf_ + cxl_header->allocator_.offset_));

  // --- 3. Initialize GIM ---
  gim = new GIMMem();
  if (gim->Init(meta_num, cxl_capacity_GiB * GiB, gim_per_meta_MiB, gim_path,
                gim_numa_node) != 0) {
    SPDLOG_ERROR("CXLDevice: GIMMem init failed");
    return -1;
  }

  // Set up per-meta allocator in DFSHeader
  dfs_header_->allocator_[meta_num].Init(gim->MyBuf(), gim_per_meta_MiB * MiB);
  dfs_header_->numa_node_[meta_num] = gim_numa_node;
  gim->SetAllocator(&dfs_header_->allocator_[meta_num]);

  // Meta count synchronization
  gim_header->meta_counts_.fetch_add(1);
  uint64_t meta_count;
  do {
    meta_count = gim_header->meta_counts_.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (meta_count != gim_header->meta_counts_.load());

  gim->SetTotalMetas(static_cast<int>(meta_count));
  gim->MapOtherMetas(dfs_header_->numa_node_);

  // --- 4. Initialize CXL SSD ---
  ssd = new CXLSSD();
  if (ssd->Init(cxl, ssd_dram_MiB, flash_path, ssd_header, meta_num) != 0) {
    SPDLOG_ERROR("CXLDevice: CXLSSD init failed");
    return -1;
  }

  // --- 5. Multi-meta ordered barrier ---
  cxl_header->meta_finish_.fetch_or(1ULL << meta_num);
  uint64_t mask = (1ULL << (meta_num + 1)) - 1;
  while ((cxl_header->meta_finish_.load() & mask) != mask) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  SPDLOG_INFO("CXLDevice: meta {} fully initialized", meta_num);
  return 0;
}

CXLDevice::~CXLDevice() {
  delete ssd;
  ssd = nullptr;
  delete gim;
  gim = nullptr;
  delete cxl;
  cxl = nullptr;
}

void InitDevice(int meta_num, uint64_t cxl_capacity_GiB,
                const std::string &cxl_path, int cxl_numa_node,
                uint64_t gim_per_meta_MiB, const std::string &gim_path,
                int gim_numa_node, uint64_t ssd_dram_MiB,
                const std::string &flash_path) {
  if (gDevice != nullptr)
    return;
  gDevice = new CXLDevice();
  if (gDevice->Init(meta_num, cxl_capacity_GiB, cxl_path, cxl_numa_node,
                    gim_per_meta_MiB, gim_path, gim_numa_node, ssd_dram_MiB,
                    flash_path) != 0) {
    SPDLOG_ERROR("CXLDevice init failed");
    exit(1);
  }
}

void DestroyDevice() {
  delete gDevice;
  gDevice = nullptr;
}

} // namespace dfs
