#include "cxl/cxl_ssd.h"

#include "spdlog/spdlog.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace dfs {

auto CXLSSD::Init(CXLMem *cxl_mem, uint64_t dram_capacity_MiB,
                  const std::string &flash_path, CXLSSDDramHeader *header,
                  int meta_num) -> int {
  cxl_mem_ = cxl_mem;
  header_ = header;

  uint64_t dram_bytes = dram_capacity_MiB * 1024ULL * 1024ULL;

  // Meta 0 allocates DRAM region from CXL memory pool
  if (meta_num == 0) {
    uint64_t dram_offset = cxl_mem_->Alloc(dram_bytes);
    header_->Init(dram_offset, dram_bytes);
    SPDLOG_INFO("CXLSSD: meta 0 allocated {}MB DRAM at CXL offset {}",
                dram_capacity_MiB, dram_offset);
  } else {
    // Wait for meta 0 to initialize
    while (!header_->IsInit()) {
    }
    SPDLOG_INFO("CXLSSD: meta {} attached to DRAM region (offset={}, cap={})",
                meta_num, header_->base_offset_, header_->capacity_);
  }

  // Open flash backend (NVMe SSD). Inode and dirent each get their own file
  // so persistence traffic is physically separated and readable offsets are
  // naturally file-local (starting from 0 in each).
  if (!flash_path.empty()) {
    std::filesystem::create_directories(flash_path);
    auto meta_prefix = flash_path + "/Meta" + std::to_string(meta_num);
    std::string inode_file = meta_prefix + "_Inode";
    std::string dent_file = meta_prefix + "_Dent";

    flash_fd_inode_ = open(inode_file.c_str(), O_RDWR | O_CREAT, 0666);
    if (flash_fd_inode_ < 0) {
      SPDLOG_ERROR("CXLSSD: open inode flash {} failed: {}", inode_file,
                   strerror(errno));
      return -1;
    }
    flash_fd_dent_ = open(dent_file.c_str(), O_RDWR | O_CREAT, 0666);
    if (flash_fd_dent_ < 0) {
      SPDLOG_ERROR("CXLSSD: open dent flash {} failed: {}", dent_file,
                   strerror(errno));
      close(flash_fd_inode_);
      flash_fd_inode_ = -1;
      return -1;
    }
    SPDLOG_INFO("CXLSSD: flash backend opened: inode={} dent={}", inode_file,
                dent_file);
  }

  initialized_ = true;
  SPDLOG_INFO("CXLSSD: initialized (meta={}, dram={}MB, flash={})", meta_num,
              dram_capacity_MiB, flash_path.empty() ? "none" : flash_path);
  return 0;
}

void CXLSSD::InodeWrite(uint64_t offset, const void *data, size_t len) {
  if (flash_fd_inode_ < 0)
    return;
  if (pwrite(flash_fd_inode_, data, len, offset) == -1) [[unlikely]] {
    SPDLOG_ERROR("{} failed: {}", "CXLSSD::InodeWrite", std::strerror(errno));
  }
}

void CXLSSD::InodeRead(uint64_t offset, void *data, size_t len) {
  if (flash_fd_inode_ < 0)
    return;
  pread(flash_fd_inode_, data, len, offset);
}

void CXLSSD::InodeSync() {
  if (flash_fd_inode_ >= 0) {
    fdatasync(flash_fd_inode_);
  }
}

void CXLSSD::DentWrite(uint64_t offset, const void *data, size_t len) {
  if (flash_fd_dent_ < 0)
    return;
  if (pwrite(flash_fd_dent_, data, len, offset) == -1) [[unlikely]] {
    SPDLOG_ERROR("{} failed: {}", "CXLSSD::DentWrite", std::strerror(errno));
  }
}

void CXLSSD::DentRead(uint64_t offset, void *data, size_t len) {
  if (flash_fd_dent_ < 0)
    return;
  pread(flash_fd_dent_, data, len, offset);
}

void CXLSSD::DentSync() {
  if (flash_fd_dent_ >= 0) {
    fdatasync(flash_fd_dent_);
  }
}

CXLSSD::~CXLSSD() {
  if (flash_fd_inode_ >= 0) {
    close(flash_fd_inode_);
    flash_fd_inode_ = -1;
  }
  if (flash_fd_dent_ >= 0) {
    close(flash_fd_dent_);
    flash_fd_dent_ = -1;
  }
}

} // namespace dfs
