#include "server/metadata.h"

// Stub for gMetaManager (defined in server_main.cc, pulled in by
// dfs-prototype-server-lib → rpc_server.cc)
std::unique_ptr<dfs::Metadata> gMetaManager = nullptr;

#include "common/metadata_types.h"
#include "cxl/device.h"
#include <byteswap.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <limits>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

using std::string;

namespace {

std::vector<Dirent> ReadDirSnapshot(dfs::Metadata *md, const char *path,
                                    uint32_t chunk_size) {
  auto dir = md->OpenDir(path, chunk_size);
  std::vector<Dirent> result;
  if (!dir.has_handle_) {
    return result;
  }

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
      auto *view =
          dfs::OffsetDirent(reinterpret_cast<DentView *>(buffer.data()), pos);
      if (!view->IsDeleted()) {
        result.emplace_back(view->id_, view->pid_, view->type_, view->name_);
      }
      pos += view->reclen_;
    }
    offset += read_size;
    if (read_size < chunk_size) {
      break;
    }
  }
  return result;
}

} // namespace

// The functions that these disabled tests test against are already used in
// latest tests, so we don't need to run them unless more bugs appear.

#define RUN_INODE_DIRENT_TEST
#define RUN_INDEX_TEST
#define RUN_PROCESS_FUNCTION_TEST

#ifdef RUN_INODE_DIRENT_TEST
// Inode tests and dir table tests are put in one TEST(),
// otherwise the 2 rocksdb instances from 2 tests will fight each other.
TEST(MetadataTest, InodeAndDirTable) {
  // inode tests
  auto fsmd = dfs::Metadata::Init("/tmp/dfs-meta/test");
  uint64_t inode_id = 0x80000000;
  dfs::Inode inode1(inode_id, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  fsmd->PutInode(inode_id, &inode1);
  dfs::Inode inode2;
  bool found = fsmd->GetInode(inode_id, &inode2);
  EXPECT_TRUE(found);
  found = fsmd->GetInode(0, &inode2);
  EXPECT_FALSE(found);
  found = fsmd->GetInode(bswap_64(inode_id), &inode1);
  EXPECT_FALSE(found);
  fsmd->DeleteInode(inode_id);
  found = fsmd->GetInode(inode_id, &inode1);
  EXPECT_FALSE(found);
  EXPECT_TRUE(memcmp(&inode1, &inode2, sizeof(dfs::Inode)) == 0);

  // dir_table tests;
  const int dt_len = 3;
  Dirent dt1[dt_len] = {Dirent(3, 2, DT_REG, "file_1"),
                        Dirent(4, 2, DT_DIR, "directory_1"),
                        Dirent(5, 2, DT_LNK, "symlink_1")};
  char data2[100];
  for (auto &dent : dt1) {
    fsmd->AddDirectoryEntry(&dent, 2); // !! entries modified !!
  }
  // data = compress_dirtable(dt1, dt_len);
  // fsmd->PutDents(2, (Dirent *)data.c_str(), data.length());
  found = fsmd->GetDents(2, reinterpret_cast<Dirent *>(data2), 100, 0);
  EXPECT_TRUE(found);
  // fsmd->DeleteDents(2);
  // found = fsmd->GetDents(2, reinterpret_cast<Dirent *>(data2), 100, 0);
  // EXPECT_FALSE(found);
  // puts("{");
  auto *dptr = reinterpret_cast<Dirent *>(data2);
  // Can't compare the whole array, because there may be aribitrary characters
  // in name field after actual name's end.
  for (int i = 0; i < dt_len; i++) {
    // printf("  {%lu, %lu, %u, %u, %s}\n", dptr->id_, dptr->off_,
    //        dptr->reclen_, dptr->type_, dptr->name_);
    EXPECT_EQ(dt1[i].reclen_, dptr->reclen_);
    EXPECT_TRUE(memcmp(dt1 + i, dptr, dt1[i].reclen_ - 1) == 0);
    dptr = dfs::NextDirent(dptr);
  }
  // puts("}\n");
}
#endif

#ifdef RUN_INDEX_TEST
TEST(MetadataTest, IndexTest) {
  auto fsmd = dfs::Metadata::Init("/tmp/dfs-meta/test2");
  dfs::Inode parent_node(2, 1, 0, 1, 2, 3, 4, 5);

  // Prepare root directory "/"

  const char *names[] = {"jam", "dfs", "index", "test"};
  for (uint64_t id = 3, parent_id = 2; id < 3 + 3; parent_id = id++) {
    // ######### mkdir() #########
    for (uint64_t j = 0; j < parent_id; j++) {
      uint64_t child = id + 10 * (parent_id - 1 - j);
      Dirent entry = Dirent(child, 2, DT_DIR, names[j]);
      fsmd->AddDirectoryEntry(&entry, parent_id);
      // const char *parent_name = parent_id == 2 ? "ROOT" : names[parent_id-2];
      // SPDLOG_INFO("{}({})/{}({})", parent_name, parent_id, names[j], child);
    }
    // TODO(PLB): we also need parent's id when operating on a directory.
    // The following read may be omitted here, but not in a real mkdir() call.
    fsmd->PutInode(parent_id, &parent_node);
    dfs::Inode dir_node(id, 1, 0, 1, 2, 3, 4, 5);
    fsmd->PutInode(id, &dir_node);
  }

  dfs::Result res;
  res = fsmd->LocateInode("/");
  EXPECT_EQ(res.id_, 2);
  res = fsmd->LocateInode("/dfs");
  EXPECT_EQ(res.id_, 3);
  res = fsmd->LocateInode("/jam");
  EXPECT_EQ(res.id_, 13);
  res = fsmd->LocateInode("/dfs/index");
  EXPECT_EQ(res.id_, 4);
  res = fsmd->LocateInode("/dfs/dfs");
  EXPECT_EQ(res.id_, 14);
  res = fsmd->LocateInode("/dfs/jam");
  EXPECT_EQ(res.id_, 24);
  res = fsmd->LocateInode("/dfs/index/test");
  EXPECT_EQ(res.id_, 5);
  res = fsmd->LocateInode("/dfs/index/index");
  EXPECT_EQ(res.id_, 15);
  res = fsmd->LocateInode("/dfs/index/dfs");
  EXPECT_EQ(res.id_, 25);
  res = fsmd->LocateInode("/dfs/index/jam");
  EXPECT_EQ(res.id_, 35);
  res = fsmd->LocateInode("/dfs/index/jam/");
  EXPECT_EQ(res.id_, 35);
  res = fsmd->LocateInode("/dfs/dfs/jam");
  EXPECT_EQ(res.id_, 0);
  res = fsmd->LocateInode("/jam/jam");
  EXPECT_EQ(res.id_, 0);

  // uint64_t id;
  // for (id = 2; id <= 5; id++) {
  //   for (uint64_t j = 0; j < id; j++) {
  //     fsmd->DeleteDents(id + (id - 1 - j) * 10);
  //     // fsmd->DeleteInode(id + (id - 1 - j) * 10);
  //   }
  // }
}
#endif

#ifdef RUN_PROCESS_FUNCTION_TEST
TEST(MetadataTest, ProcessFunction) {
  auto fsmd = dfs::Metadata::Init("/tmp/dfs-meta/test3");
  dfs::Result ret;
  // mkdir
  ret = fsmd->Mkdir("/tmp", 0775, 22, 33);
  ASSERT_EQ(ret.id_, 0);
  ret = fsmd->Mkdir("/tmp/not/exist", 0555, 0, 0);
  ASSERT_EQ(ret.id_, -1);
  // access()
  ret = fsmd->Access("/tmp", F_OK, 0, 0);
  EXPECT_EQ(ret.id_, 0);
  ret = fsmd->Access("/tmp", W_OK | R_OK | X_OK, 0, 0);
  EXPECT_EQ(ret.id_, -1);
  ret = fsmd->Access("/tmp", W_OK | R_OK | X_OK, 0, 33);
  EXPECT_EQ(ret.id_, 0);
  // stat()
  dfs::Inode tmp;
  ret = fsmd->Stat("/tmp", &tmp);
  EXPECT_EQ(ret.id_, 0);
  EXPECT_EQ(tmp.mode_, 0775 | S_IFDIR);
  EXPECT_EQ(tmp.uid_, 22);
  ret = fsmd->Stat("/test", &tmp);
  EXPECT_EQ(ret.id_, -1);
  // rename()
  bool ret2;
  ret2 = fsmd->Rename("/tmp", "/test");
  ASSERT_EQ(ret2, 0);
  ret2 = fsmd->Rename("/tmp", "/test");
  ASSERT_EQ(ret2, -1);
  ret = fsmd->Access("/tmp", F_OK, 0, 0);
  EXPECT_EQ(ret.id_, -1);
  ret = fsmd->Access("/test", F_OK, 0, 0);
  EXPECT_EQ(ret.id_, 0);
  // create()
  ret = fsmd->Create("/test/file.txt", 0755, 22, 33, &tmp);
  ASSERT_EQ(ret.id_, 0);
  EXPECT_GT(tmp.id_, 2);
  SPDLOG_INFO("created inode_id = {}", tmp.id_);
  EXPECT_EQ(tmp.mode_, 0755);
  ret = fsmd->Create("/tmp/file.txt", 0755, 22, 33, &tmp);
  EXPECT_EQ(ret.id_, -1);
  ret = fsmd->Access("/test/file.txt", F_OK, 0, 0);
  EXPECT_EQ(ret.id_, 0);
  // link()
  ret2 = fsmd->Link("/test/file.txt", "/file1.txt");
  ASSERT_EQ(ret2, 0);
  dfs::Result ret5 = fsmd->LocateInode("/test/file.txt");
  dfs::Result ret6 = fsmd->LocateInode("/file1.txt");
  ASSERT_EQ(ret5.id_, ret6.id_);

  // opendir()
  auto ret3 = fsmd->OpenDir("/", 30);
  ASSERT_TRUE(ret3.has_handle_);
  EXPECT_EQ(ret3.handle_.id_, dfs::kRootId);

  // readdir()
  int ret4 = 0;
  for (const auto &dent : ReadDirSnapshot(fsmd.get(), "/", 30)) {
    if (ret4 >= 4) {
      break;
    }
    ret4++;
    SPDLOG_INFO("Entry #{}: id = {}, name = {}, reclen = {}", ret4, dent.id_,
                dent.name_, dent.reclen_);
  }
  EXPECT_EQ(ret4, 2);

  // unlink()
  ret = fsmd->Unlink("/test/file.txt");
  ASSERT_EQ(ret.id_, 0);
  ret = fsmd->Access("/test/file.txt", F_OK, 0, 0);
  EXPECT_EQ(ret.id_, -1);
  ret = fsmd->Access("/file1.txt", F_OK, 0, 0);
  EXPECT_EQ(ret.id_, 0);
  ret = fsmd->Unlink("/test");
  ASSERT_EQ(ret.id_, -1);
  // rmdir()
  ret = fsmd->Rmdir("/test");
  EXPECT_EQ(ret.id_, 0);
  ret = fsmd->Access("/test", F_OK, 0, 0);
  EXPECT_EQ(ret.id_, -1);
  ret3 = fsmd->OpenDir("/", 64);
  ASSERT_TRUE(ret3.has_handle_);
  ret4 = 0;
  for (const auto &dent : ReadDirSnapshot(fsmd.get(), "/", 64)) {
    if (ret4 >= 3) {
      break;
    }
    ret4++;
    SPDLOG_INFO("Entry #{}: id = {}, name = {}, reclen = {}", ret4, dent.id_,
                dent.name_, dent.reclen_);
  }
  EXPECT_EQ(ret4, 1);

  ret = fsmd->Unlink("/file1.txt");
  ASSERT_EQ(ret.id_, 0);

  ret3 = fsmd->OpenDir("/", 64);
  EXPECT_TRUE(ReadDirSnapshot(fsmd.get(), "/", 64).empty());
}
#endif
