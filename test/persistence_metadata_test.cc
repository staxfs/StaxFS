#include "server/metadata.h"

// Stub for gMetaManager (defined in server_main.cc, pulled in by
// dfs-prototype-server-lib → rpc_server.cc)
std::unique_ptr<dfs::Metadata> gMetaManager = nullptr;

#include "server/cxl_persistence.h"
#include "test_cxl_helper.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

class MetadataPersistenceTestScope {
public:
  explicit MetadataPersistenceTestScope(const std::string &root)
      : root_(root), cxl_(768 * 1024 * 1024, 512) {
    std::filesystem::remove_all(root_);
    dfs::InitCXLPersistence(0);
  }

  ~MetadataPersistenceTestScope() {
    dfs::DestroyCXLPersistence();
    std::filesystem::remove_all(root_);
  }

private:
  std::string root_;
  dfs::test::TestCXLSetup cxl_;
};

bool WaitForCheckpointToReach(uint64_t target) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (dfs::gCXLPersistence != nullptr &&
        dfs::gCXLPersistence->WAL()->CheckpointPos() >= target) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

dfs::Inode ReadPersistedInode(uint64_t inode_id) {
  dfs::Inode inode{};
  dfs::gCXLPersistence->InodeRegion()->ReadInode(inode_id, &inode,
                                                 sizeof(inode));
  return inode;
}

std::vector<std::string> ReadLocalDirNames(dfs::Metadata *md,
                                           const char *path,
                                           uint32_t chunk_size) {
  auto dir = md->OpenDir(path, chunk_size);
  EXPECT_TRUE(dir.has_handle_);

  std::vector<std::string> names;
  std::vector<char> buffer(chunk_size);
  int offset = 0;
  while (true) {
    uint64_t read_size = md->GetDentViews(
        dir.handle_.id_, std::numeric_limits<uint64_t>::max(),
        reinterpret_cast<DentView *>(buffer.data()), chunk_size, offset);
    if (read_size == 0) {
      break;
    }
    size_t pos = 0;
    while (pos + offsetof(DentView, name_) <= read_size) {
      auto *view = dfs::OffsetDirent(
          reinterpret_cast<DentView *>(buffer.data()), pos);
      EXPECT_GE(view->reclen_, offsetof(DentView, name_) + 1);
      EXPECT_LE(pos + view->reclen_, read_size);
      if (!view->IsDeleted()) {
        names.emplace_back(view->name_);
      }
      pos += view->reclen_;
    }
    EXPECT_EQ(pos, read_size);
    offset += read_size;
    if (read_size < chunk_size) {
      break;
    }
  }
  return names;
}

TEST(PersistenceMetadataTest, CheckpointAndWalTailBackedReadDir) {
  MetadataPersistenceTestScope scope("/tmp/dfs-meta/persistence-test");
  auto fsmd = dfs::Metadata::Init("/tmp/dfs-meta/persistence-test");

  dfs::Inode inode;
  ASSERT_EQ(fsmd->Mkdir("/dir", 0755, 1, 1).mark_, 0);
  ASSERT_EQ(fsmd->Create("/dir/file.txt", 0644, 1, 1, &inode).mark_, 0);
  ASSERT_EQ(fsmd->Create("/dir/other.txt", 0644, 1, 1, &inode).mark_, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  ASSERT_NE(dfs::gCXLPersistence, nullptr);
  EXPECT_GT(dfs::gCXLPersistence->WAL()->CheckpointPos(), 0);

  std::vector<std::string> names = ReadLocalDirNames(fsmd.get(), "/dir", 256);

  std::sort(names.begin(), names.end());
  ASSERT_EQ(names.size(), 2);
  EXPECT_EQ(names[0], "file.txt");
  EXPECT_EQ(names[1], "other.txt");

  ASSERT_EQ(fsmd->Unlink("/dir/file.txt").mark_, 0);
  names = ReadLocalDirNames(fsmd.get(), "/dir", 256);

  ASSERT_EQ(names.size(), 1);
  EXPECT_EQ(names[0], "other.txt");
}

TEST(PersistenceMetadataTest, ReplaysWalIntoPersistedInodeSnapshots) {
  MetadataPersistenceTestScope scope("/tmp/dfs-meta/persistence-inode-test");
  auto fsmd = dfs::Metadata::Init("/tmp/dfs-meta/persistence-inode-test");

  ASSERT_NE(dfs::gCXLPersistence, nullptr);

  dfs::Inode created;
  ASSERT_EQ(fsmd->Create("/file.txt", 0644, 11, 22, &created).mark_, 0);
  uint64_t checkpoint_target = dfs::gCXLPersistence->WAL()->Head();
  ASSERT_TRUE(WaitForCheckpointToReach(checkpoint_target));

  auto persisted = ReadPersistedInode(created.id_);
  EXPECT_EQ(persisted.id_, created.id_);
  EXPECT_EQ(persisted.nlink_, 1);
  EXPECT_EQ(persisted.mode_, static_cast<mode_t>(0644 | S_IFREG));
  EXPECT_EQ(persisted.uid_, 11U);
  EXPECT_EQ(persisted.gid_, 22U);

  ASSERT_EQ(fsmd->Chmod("/file.txt", 0600, 11, 22).mark_, 0);
  ASSERT_EQ(fsmd->Link("/file.txt", "/file-link"), 0);
  checkpoint_target = dfs::gCXLPersistence->WAL()->Head();
  ASSERT_TRUE(WaitForCheckpointToReach(checkpoint_target));

  persisted = ReadPersistedInode(created.id_);
  EXPECT_EQ(persisted.mode_, static_cast<mode_t>(0600));
  EXPECT_EQ(persisted.nlink_, 2);

  ASSERT_EQ(fsmd->Unlink("/file-link").mark_, 0);
  checkpoint_target = dfs::gCXLPersistence->WAL()->Head();
  ASSERT_TRUE(WaitForCheckpointToReach(checkpoint_target));

  persisted = ReadPersistedInode(created.id_);
  EXPECT_EQ(persisted.id_, created.id_);
  EXPECT_EQ(persisted.nlink_, 1);

  ASSERT_EQ(fsmd->Unlink("/file.txt").mark_, 0);
  checkpoint_target = dfs::gCXLPersistence->WAL()->Head();
  ASSERT_TRUE(WaitForCheckpointToReach(checkpoint_target));

  // Soft delete: inode stays on SSD with nlink=0 (not hard-deleted) so that
  // out-of-order remote LINK operations can still find and increment it.
  persisted = ReadPersistedInode(created.id_);
  EXPECT_EQ(persisted.id_, created.id_);
  EXPECT_EQ(persisted.nlink_, 0U);
}

TEST(PersistenceMetadataTest, PacksShortNamesIntoSingleDirPage) {
  MetadataPersistenceTestScope scope(
      "/tmp/dfs-meta/persistence-packed-dir-test");
  auto fsmd = dfs::Metadata::Init("/tmp/dfs-meta/persistence-packed-dir-test");

  ASSERT_NE(dfs::gCXLPersistence, nullptr);
  ASSERT_EQ(fsmd->Mkdir("/packed", 0755, 1, 1).mark_, 0);

  dfs::Inode inode;
  for (int i = 0; i < 80; ++i) {
    std::string path = "/packed/f" + std::to_string(i);
    ASSERT_EQ(fsmd->Create(path.c_str(), 0644, 1, 1, &inode).mark_, 0);
  }

  uint64_t checkpoint_target = dfs::gCXLPersistence->WAL()->Head();
  ASSERT_TRUE(WaitForCheckpointToReach(checkpoint_target));
  EXPECT_EQ(dfs::gCXLPersistence->DentRegion()->AllocPos(),
            dfs::DirPage::kPageSize * 3);

  int count = static_cast<int>(ReadLocalDirNames(fsmd.get(), "/packed", 4096).size());

  EXPECT_EQ(count, 80);
}

} // namespace
