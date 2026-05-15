#include "server/level_hashtable.h"
#include "server/level_hashtable_traits.h"
#include "test_cxl_helper.h"

// Stub for gMetaManager (defined in server_main.cc, pulled in by
// dfs-prototype-server-lib → rpc_server.cc)
#include "server/metadata.h"
std::unique_ptr<dfs::Metadata> gMetaManager = nullptr;

#include <algorithm>
#include <cstdlib>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace dfs;

// ─── Global CXL test setup (shared across all tests) ──────────────
static dfs::test::TestDualMemSetup *gCXLSetup = nullptr;

class CXLEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    gCXLSetup = new dfs::test::TestDualMemSetup(256 * 1024 * 1024);
  }

  void TearDown() override {
    delete gCXLSetup;
    gCXLSetup = nullptr;
  }
};

// Register global environment
static auto *const gEnv =
    ::testing::AddGlobalTestEnvironment(new CXLEnvironment);

// ─── Dirent helpers ────────────────────────────────────────────────
static Dirent make_dirent(uint64_t id, uint64_t pid, const char *name) {
  Dirent d{};
  d.id_ = id;
  d.pid_ = pid;
  d.type_ = 8; // DT_REG
  std::strncpy(d.name_, name, sizeof(d.name_) - 1);
  d.SetRecLen();
  return d;
}

// ─── Inode helpers ─────────────────────────────────────────────────
static Inode make_inode(uint64_t id) {
  time_t now = time(nullptr);
  return Inode(id, 1, S_IFREG | 0644, 1000, 1000, now, now, now);
}

// ═══════════════════════════════════════════════════════════════════
// Basic CRUD Tests — DirentHashtable
// ═══════════════════════════════════════════════════════════════════

class DirentHashtableTest : public ::testing::Test {
protected:
  static constexpr size_t kTLBuckets = 64;
  DirentHashtable *ht_ = nullptr;

  void SetUp() override {
    gCXLSetup->reset();
    ht_ = new DirentHashtable(kTLBuckets);
  }

  void TearDown() override { delete ht_; }
};

TEST_F(DirentHashtableTest, InsertAndFind) {
  DKeyPair key{100, "hello.txt"};
  Dirent d = make_dirent(1, 100, "hello.txt");

  ASSERT_EQ(ht_->insert(key, d), 1);
  ASSERT_EQ(ht_->GetElementNum(), 1u);

  Dirent out{};
  ASSERT_TRUE(ht_->find(key, out));
  EXPECT_EQ(out.id_, 1u);
  EXPECT_EQ(out.pid_, 100u);
  EXPECT_STREQ(out.name_, "hello.txt");
}

TEST_F(DirentHashtableTest, FindNotFound) {
  DKeyPair key{999, "nonexistent"};
  Dirent out{};
  EXPECT_FALSE(ht_->find(key, out));
}

TEST_F(DirentHashtableTest, InsertMultiple) {
  constexpr int N = 100;
  for (int i = 0; i < N; ++i) {
    std::string name = "file_" + std::to_string(i);
    DKeyPair key{1, name};
    Dirent d = make_dirent(i + 10, 1, name.c_str());
    ASSERT_EQ(ht_->insert(key, d), 1)
        << "Failed at i=" << i;
  }
  EXPECT_EQ(ht_->GetElementNum(), N);

  // Verify all
  for (int i = 0; i < N; ++i) {
    std::string name = "file_" + std::to_string(i);
    DKeyPair key{1, name};
    Dirent out{};
    ASSERT_TRUE(ht_->find(key, out)) << "Not found: " << name;
    EXPECT_EQ(out.id_, static_cast<uint64_t>(i + 10));
  }
}

TEST_F(DirentHashtableTest, Update) {
  DKeyPair key{100, "test.txt"};
  Dirent d = make_dirent(1, 100, "test.txt");
  ht_->insert(key, d);

  // Update: change inode id
  d.id_ = 42;
  ASSERT_TRUE(ht_->update(key, d));

  Dirent out{};
  ASSERT_TRUE(ht_->find(key, out));
  EXPECT_EQ(out.id_, 42u);
}

TEST_F(DirentHashtableTest, UpdateFn) {
  DKeyPair key{100, "test.txt"};
  Dirent d = make_dirent(1, 100, "test.txt");
  ht_->insert(key, d);

  // update_fn: modify via callback
  ASSERT_TRUE(ht_->update_fn(key, [](Dirent &d) { d.id_ = 99; }));

  Dirent out{};
  ASSERT_TRUE(ht_->find(key, out));
  EXPECT_EQ(out.id_, 99u);
}

TEST_F(DirentHashtableTest, Erase) {
  DKeyPair key{100, "delete_me.txt"};
  Dirent d = make_dirent(7, 100, "delete_me.txt");
  ht_->insert(key, d);

  Dirent erased{};
  ASSERT_TRUE(ht_->erase(key, erased));
  EXPECT_EQ(erased.id_, 7u);
  EXPECT_EQ(ht_->GetElementNum(), 0u);

  // Should not find anymore
  Dirent out{};
  EXPECT_FALSE(ht_->find(key, out));
}

TEST_F(DirentHashtableTest, EraseNotFound) {
  DKeyPair key{999, "ghost"};
  EXPECT_FALSE(ht_->erase(key));
}

TEST_F(DirentHashtableTest, InsertEraseInsert) {
  DKeyPair key{1, "reuse"};
  Dirent d1 = make_dirent(10, 1, "reuse");
  Dirent d2 = make_dirent(20, 1, "reuse");

  ASSERT_EQ(ht_->insert(key, d1), 1);
  ASSERT_TRUE(ht_->erase(key));
  ASSERT_EQ(ht_->insert(key, d2), 1);

  Dirent out{};
  ASSERT_TRUE(ht_->find(key, out));
  EXPECT_EQ(out.id_, 20u);
}

TEST_F(DirentHashtableTest, LoadFactor) {
  EXPECT_DOUBLE_EQ(ht_->load_factor(), 0.0);
  DKeyPair key{1, "x"};
  ht_->insert(key, make_dirent(1, 1, "x"));
  EXPECT_GT(ht_->load_factor(), 0.0);
}

// ═══════════════════════════════════════════════════════════════════
// Basic CRUD Tests — InodeHashtable
// ═══════════════════════════════════════════════════════════════════

class InodeHashtableTest : public ::testing::Test {
protected:
  static constexpr size_t kTLBuckets = 64;
  InodeHashtable *ht_ = nullptr;

  void SetUp() override {
    gCXLSetup->reset();
    ht_ = new InodeHashtable(kTLBuckets);
  }

  void TearDown() override { delete ht_; }
};

TEST_F(InodeHashtableTest, InsertFindUpdateErase) {
  uint64_t key = 12345;
  Inode inode = make_inode(key);
  inode.size_ = 4096;

  ASSERT_EQ(ht_->insert(key, inode), 1);

  Inode out{};
  ASSERT_TRUE(ht_->find(key, out));
  EXPECT_EQ(out.id_, key);
  EXPECT_EQ(out.size_, 4096u);

  // Update size
  ASSERT_TRUE(ht_->update_fn(key, [](Inode &i) { i.size_ = 8192; }));
  ASSERT_TRUE(ht_->find(key, out));
  EXPECT_EQ(out.size_, 8192u);

  // Erase
  ASSERT_TRUE(ht_->erase(key));
  EXPECT_FALSE(ht_->find(key, out));
}

TEST_F(InodeHashtableTest, ManyInodes) {
  constexpr int N = 200;
  for (int i = 1; i <= N; ++i) {
    Inode inode = make_inode(i);
    inode.size_ = i * 100;
    ASSERT_EQ(ht_->insert(static_cast<uint64_t>(i), inode),
              1);
  }
  EXPECT_EQ(ht_->GetElementNum(), N);

  for (int i = 1; i <= N; ++i) {
    Inode out{};
    ASSERT_TRUE(ht_->find(static_cast<uint64_t>(i), out));
    EXPECT_EQ(out.size_, static_cast<size_t>(i * 100));
  }
}

// ═══════════════════════════════════════════════════════════════════
// Movement (Migration) Tests
// ═══════════════════════════════════════════════════════════════════

TEST_F(DirentHashtableTest, HighLoadFactor) {
  // Fill up to near capacity and verify movement works. Both the optimized
  // 8-slot layout and the baseline 4-slot layout should absorb ≥65% of the
  // table's raw slot capacity before insert starts failing.
  const size_t space = ht_->GetSpaceNum();
  int inserted = 0;
  for (int pid = 1; pid <= 10; ++pid) {
    for (int i = 0; i < 60; ++i) {
      std::string name = "f_" + std::to_string(pid) + "_" + std::to_string(i);
      DKeyPair key{static_cast<uint64_t>(pid), name};
      auto status =
          ht_->insert(key, make_dirent(inserted + 1, static_cast<uint64_t>(pid),
                                       name.c_str()));
      if (status == 1) {
        ++inserted;
      } else {
        break;
      }
    }
  }
  SPDLOG_INFO("Inserted {} entries, load_factor={:.3f}, space={}", inserted,
              ht_->load_factor(), space);
  EXPECT_GT(static_cast<double>(inserted), 0.65 * static_cast<double>(space));

  // Verify all inserted entries are findable
  int found = 0;
  for (int pid = 1; pid <= 10; ++pid) {
    for (int i = 0; i < 60; ++i) {
      std::string name = "f_" + std::to_string(pid) + "_" + std::to_string(i);
      DKeyPair key{static_cast<uint64_t>(pid), name};
      Dirent out{};
      if (ht_->find(key, out))
        ++found;
    }
  }
  EXPECT_EQ(found, inserted);
}

// ═══════════════════════════════════════════════════════════════════
// Concurrent Tests
// ═══════════════════════════════════════════════════════════════════

TEST(ConcurrentTest, ParallelInsertFind) {
  constexpr size_t kTLBuckets = 256;
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 200;

  gCXLSetup->reset();
  InodeHashtable ht(kTLBuckets);

  // Phase 1: parallel insert
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&ht, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        uint64_t key = t * kOpsPerThread + i + 1;
        Inode inode = make_inode(key);
        auto status = ht.insert(key, inode);
        EXPECT_EQ(status, 1);
      }
    });
  }
  for (auto &t : threads)
    t.join();
  threads.clear();

  EXPECT_EQ(ht.GetElementNum(),
            static_cast<uint64_t>(kThreads * kOpsPerThread));

  // Phase 2: parallel find
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&ht, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        uint64_t key = t * kOpsPerThread + i + 1;
        Inode out{};
        EXPECT_TRUE(ht.find(key, out));
        EXPECT_EQ(out.id_, key);
      }
    });
  }
  for (auto &t : threads)
    t.join();
}

TEST(ConcurrentTest, ParallelInsertErase) {
  constexpr size_t kTLBuckets = 256;
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 100;

  gCXLSetup->reset();
  InodeHashtable ht(kTLBuckets);

  // Insert all
  for (int i = 1; i <= kThreads * kOpsPerThread; ++i) {
    ht.insert(static_cast<uint64_t>(i), make_inode(i));
  }

  // Parallel erase
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&ht, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        uint64_t key = t * kOpsPerThread + i + 1;
        EXPECT_TRUE(ht.erase(key));
      }
    });
  }
  for (auto &t : threads)
    t.join();

  EXPECT_EQ(ht.GetElementNum(), 0u);
}

TEST(ConcurrentTest, MixedReadWrite) {
  constexpr size_t kTLBuckets = 256;
  constexpr int kWriters = 2;
  constexpr int kReaders = 2;
  constexpr int kOps = 200;

  gCXLSetup->reset();
  InodeHashtable ht(kTLBuckets);

  std::atomic<bool> done{false};

  // Writers: insert and update
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&ht, w, &done]() {
      for (int i = 0; i < kOps; ++i) {
        uint64_t key = w * kOps + i + 1;
        Inode inode = make_inode(key);
        inode.size_ = i;
        ht.insert(key, inode);
        // Update
        ht.update_fn(key, [&](Inode &n) { n.size_ = i * 2; });
      }
      done.store(true, std::memory_order_release);
    });
  }

  // Readers: find continuously
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&ht, &done]() {
      while (!done.load(std::memory_order_acquire)) {
        for (uint64_t k = 1; k <= kOps * 2; ++k) {
          Inode out{};
          ht.find(k, out); // may or may not find
        }
      }
    });
  }

  for (auto &t : threads)
    t.join();
}

// ═══════════════════════════════════════════════════════════════════
// Edge Cases
// ═══════════════════════════════════════════════════════════════════

TEST_F(InodeHashtableTest, DuplicateKeyInsert) {
  uint64_t key = 42;
  Inode i1 = make_inode(key);
  i1.size_ = 100;
  Inode i2 = make_inode(key);
  i2.size_ = 200;

  ASSERT_EQ(ht_->insert(key, i1), 1);
  ASSERT_EQ(ht_->insert(key, i2), 1);
  EXPECT_EQ(ht_->GetElementNum(), 2u);

  Inode out{};
  ASSERT_TRUE(ht_->find(key, out));
  EXPECT_TRUE(out.size_ == 100 || out.size_ == 200);
}

TEST_F(InodeHashtableTest, UpdateNonExistent) {
  EXPECT_FALSE(ht_->update(999ULL, make_inode(999)));
}

TEST_F(InodeHashtableTest, ZeroHashKey) {
  uint64_t key = 0;
  Inode inode = make_inode(key);
  ASSERT_EQ(ht_->insert(key, inode), 1);
  Inode out{};
  ASSERT_TRUE(ht_->find(key, out));
  EXPECT_EQ(out.id_, 0u);
}

// ═══════════════════════════════════════════════════════════════════
// TagHint Tests
// ═══════════════════════════════════════════════════════════════════

#ifdef LEVEL_HASHTABLE_TAG_HINT
TEST(TagHintTest, BasicSetTestClear) {
  dfs::TagHint hint(100);
  constexpr uint64_t kFpA = 0x123456789ABCULL;
  constexpr uint64_t kFpB = 0xFEDCBA987654ULL;

  EXPECT_FALSE(hint.test(0, kFpA));
  hint.set(0, /*slot_idx=*/3, kFpA);
  EXPECT_TRUE(hint.test(0, kFpA));
  EXPECT_FALSE(hint.test(0, kFpB));

  // Clearing a different slot must not affect slot 3.
  hint.clear(0, /*slot_idx=*/0);
  EXPECT_TRUE(hint.test(0, kFpA));

  // Clearing the exact slot drops the tag.
  hint.clear(0, /*slot_idx=*/3);
  EXPECT_FALSE(hint.test(0, kFpA));

  // Multiple slots in the same bucket coexist without bit accumulation.
  hint.set(0, /*slot_idx=*/1, kFpA);
  hint.set(0, /*slot_idx=*/5, kFpB);
  EXPECT_TRUE(hint.test(0, kFpA));
  EXPECT_TRUE(hint.test(0, kFpB));
  hint.clear(0, /*slot_idx=*/1);
  EXPECT_FALSE(hint.test(0, kFpA));
  EXPECT_TRUE(hint.test(0, kFpB));
}

TEST(TagHintTest, IntegrationWithHashtable) {
  constexpr size_t kTLBuckets = 64;

  gCXLSetup->reset();
  InodeHashtable ht(kTLBuckets);

  // Insert
  for (int i = 1; i <= 50; ++i) {
    ht.insert(static_cast<uint64_t>(i), make_inode(i));
  }

  // Find should work with hint
  for (int i = 1; i <= 50; ++i) {
    Inode out{};
    ASSERT_TRUE(ht.find(static_cast<uint64_t>(i), out));
    EXPECT_EQ(out.id_, static_cast<uint64_t>(i));
  }

  // Erase and verify hint cleared
  ht.erase(25ULL);
  Inode out{};
  EXPECT_FALSE(ht.find(25ULL, out));
}
#endif

// ═══════════════════════════════════════════════════════════════════
// GIM + Resize Tests
// ═══════════════════════════════════════════════════════════════════

TEST(ResizeTest, SingleMDSResize) {
  constexpr size_t kTLBuckets = 16;

  gCXLSetup->reset();
  InodeHashtable ht(kTLBuckets);

  // Allocate ResizeMeta in CXL memory (not stack)
  uint64_t meta_off = gDevice->CXLMemMalloc(sizeof(dfs::ResizeMeta));
  gDevice->CXLMemReset(meta_off, sizeof(dfs::ResizeMeta));
  ht.enable_resize({meta_off, 0, 1, nullptr});

  // Fill hash table — resize triggers automatically via insert
  int inserted = 0;
  for (uint64_t i = 1; i <= 1000; ++i) {
    auto s = ht.insert(i, make_inode(i));
    if (s == 1) {
      ++inserted;
    } else {
      break;
    }
  }
  SPDLOG_INFO("Post-resize: inserted={}, tl={}, bl={}", inserted,
              ht.tl_num_buckets(), ht.bl_num_buckets());

  // Resize should have doubled TL at least once
  EXPECT_GT(ht.tl_num_buckets(), kTLBuckets);

  // Verify all inserted entries are findable
  int found = 0;
  for (uint64_t i = 1; i <= static_cast<uint64_t>(inserted); ++i) {
    dfs::Inode out{};
    if (ht.find(i, out))
      ++found;
  }
  EXPECT_EQ(found, inserted);
}

// Regression test for the mdtest failure mode: insert many unique dirents
// under a handful of parent ids, with resize enabled. Every insert must
// succeed — either directly or via an automatic resize on insert failure.
//
// Originally, the CLH baseline's `do_resize()` had a
// `if (load_factor <= threshold) return;` early-return that masked real
// insert failures at low-to-moderate global load: when `insert_impl`
// genuinely failed due to local bucket clustering, do_resize would bail
// without resizing, the caller would retry with the same layout, and the
// second attempt would fail the same way — producing "DHashtable Insert
// [pid, name] FAILED" log spam during mdtest even though the hash table
// was nowhere near full. This test exercises the exact pattern (same pid,
// dir.mdtest.0.N names) at a scale that triggers at least one resize.
TEST(ResizeTest, MdtestLikeInsertNeverFails) {
  constexpr size_t kTLBuckets = 16; // small on purpose → forces resizes

  gCXLSetup->reset();
  DirentHashtable ht(kTLBuckets);

  uint64_t meta_off = gDevice->CXLMemMalloc(sizeof(dfs::ResizeMeta));
  gDevice->CXLMemReset(meta_off, sizeof(dfs::ResizeMeta));
  ht.enable_resize({meta_off, 0, 1, nullptr});

  constexpr int kNumPids = 4;
  constexpr int kEntriesPerPid = 600;
  int inserted = 0;
  int id = 1;
  for (int pid = 10; pid < 10 + kNumPids; ++pid) {
    for (int i = 0; i < kEntriesPerPid; ++i) {
      std::string name = "dir.mdtest.0." + std::to_string(i);
      DKeyPair key{static_cast<uint64_t>(pid), name};
      ASSERT_EQ(ht.insert(key, make_dirent(id, static_cast<uint64_t>(pid),
                                           name.c_str())),
                1)
          << "insert failed at pid=" << pid << " name=" << name
          << " inserted_so_far=" << inserted
          << " tl=" << ht.tl_num_buckets() << " bl=" << ht.bl_num_buckets();
      ++inserted;
      ++id;
    }
  }
  SPDLOG_INFO("MdtestLikeInsertNeverFails: inserted={}, tl={}, bl={}", inserted,
              ht.tl_num_buckets(), ht.bl_num_buckets());
  EXPECT_EQ(inserted, kNumPids * kEntriesPerPid);
  EXPECT_GT(ht.tl_num_buckets(), kTLBuckets);

  // Verify every entry is still findable.
  int found = 0;
  int id2 = 1;
  for (int pid = 10; pid < 10 + kNumPids; ++pid) {
    for (int i = 0; i < kEntriesPerPid; ++i) {
      std::string name = "dir.mdtest.0." + std::to_string(i);
      DKeyPair key{static_cast<uint64_t>(pid), name};
      Dirent out{};
      if (ht.find(key, out) && out.id_ == static_cast<uint64_t>(id2)) {
        ++found;
      }
      ++id2;
    }
  }
  EXPECT_EQ(found, inserted);
}
