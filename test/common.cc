#include <common/config.h>
#include <gtest/gtest.h>

TEST(CommonTest, ParseCoreIdsTest) {
  {
    // Single core
    auto core_ids = dfs::ParseCoreIds("1");
    ASSERT_EQ(core_ids.size(), 1);
    ASSERT_EQ(core_ids[0], 1);
  }

  {
    // Multiple cores
    auto core_ids = dfs::ParseCoreIds("1,2,3");
    ASSERT_EQ(core_ids.size(), 3);
    ASSERT_EQ(core_ids[0], 1);
    ASSERT_EQ(core_ids[1], 2);
    ASSERT_EQ(core_ids[2], 3);
  }

  {
    // Range
    auto core_ids = dfs::ParseCoreIds("1-3");
    ASSERT_EQ(core_ids.size(), 3);
    ASSERT_EQ(core_ids[0], 1);
    ASSERT_EQ(core_ids[1], 2);
    ASSERT_EQ(core_ids[2], 3);
  }

  {
    // Multiple ranges
    auto core_ids = dfs::ParseCoreIds("1-3,5-6");
    ASSERT_EQ(core_ids.size(), 5);
    ASSERT_EQ(core_ids[0], 1);
    ASSERT_EQ(core_ids[1], 2);
    ASSERT_EQ(core_ids[2], 3);
    ASSERT_EQ(core_ids[3], 5);
    ASSERT_EQ(core_ids[4], 6);
  }

  {
    // Multiple ranges with single core
    auto core_ids = dfs::ParseCoreIds("1-3,5-6,8");
    ASSERT_EQ(core_ids.size(), 6);
    ASSERT_EQ(core_ids[0], 1);
    ASSERT_EQ(core_ids[1], 2);
    ASSERT_EQ(core_ids[2], 3);
    ASSERT_EQ(core_ids[3], 5);
    ASSERT_EQ(core_ids[4], 6);
    ASSERT_EQ(core_ids[5], 8);
  }
}