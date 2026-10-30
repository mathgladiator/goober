#include <gtest/gtest.h>

#include "goober/bus/bus.h"

using goober::bus::Bus;

TEST(Bus, SequencesOnlyMoveForwardFromTheSeed) {
  Bus bus(1000);
  EXPECT_EQ(bus.seq("a"), 0);
  EXPECT_EQ(bus.publish("a", 1), 1001);
  EXPECT_EQ(bus.publish("b", 2), 1002);
  EXPECT_EQ(bus.publish("a", 3), 1003);
  EXPECT_EQ(bus.seq("a"), 1003);
  EXPECT_EQ(bus.max_seq({"a", "b", "nope"}), 1003);
  // A bus started later (a restart) seeded from a later time is ahead of everything before.
  Bus later(2000);
  EXPECT_GT(later.publish("a", 4), 1003);
}

TEST(Bus, WatchersHearTheirTopicsUntilTheyStop) {
  Bus bus(0);
  std::vector<std::pair<std::string, int64_t>> heard;
  auto id = bus.watch({"t1/orders", "t1/people"}, [&](const std::string& t, int64_t s) { heard.emplace_back(t, s); });
  bus.publish("t1/orders", 1);
  bus.publish("t2/orders", 1);  // another tenant's: not ours
  bus.publish("t1/people", 1);
  ASSERT_EQ(heard.size(), 2u);
  EXPECT_EQ(heard[0].first, "t1/orders");
  EXPECT_EQ(heard[1].first, "t1/people");
  EXPECT_EQ(bus.watchers(), 1u);
  bus.unwatch(id);
  bus.publish("t1/orders", 2);
  EXPECT_EQ(heard.size(), 2u);
  EXPECT_EQ(bus.watchers(), 0u);
}

TEST(Bus, AWakeMayPublishWithoutDeadlockAndTopicsAreBounded) {
  Bus bus(0, 3);
  bus.watch({"a"}, [&](const std::string&, int64_t) { bus.publish("b", 1); });
  bus.publish("a", 1);
  EXPECT_GT(bus.seq("b"), bus.seq("a"));
  bus.publish("c", 2);
  bus.publish("d", 3);  // four topics: the one published longest ago is forgotten
  EXPECT_EQ(bus.topics("", 10).size(), 3u);
  EXPECT_EQ(bus.seq("a"), 0);
  EXPECT_EQ(bus.topics("", 10).front().name, "d");
}
