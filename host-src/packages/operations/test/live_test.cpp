#include <gtest/gtest.h>

#include "goober/operations/live.h"

using goober::operations::LiveStats;

namespace {
constexpr int64_t kT0 = 1793232000000;
constexpr int64_t kMin = 60000;
}  // namespace

TEST(LiveStats, PercentilesPerPathOverTheWindow) {
  LiveStats s;
  for (int i = 0; i < 99; ++i) s.record(kT0, "acme", "/a", "a_route", "box", 200, 1000, "");
  s.record(kT0, "acme", "/a", "", "", 500, 2'000'000, "");
  s.record(kT0, "acme", "/b", "", "", 404, 50, "/a");
  auto rows = s.stats(kT0 + kMin, 5, "acme", "");
  ASSERT_EQ(rows.size(), 2u);
  const auto& a = rows[0].path == "/a" ? rows[0] : rows[1];
  EXPECT_EQ(a.n, 100);
  EXPECT_EQ(a.n5, 1);
  EXPECT_EQ(a.route, "a_route");
  EXPECT_LE(a.p50_us, 1500);
  EXPECT_GE(a.p50_us, 1000);
  EXPECT_LE(a.p99_us, 1500);  // the one slow call is the 100th of 100
  EXPECT_EQ(a.max_us, 2'000'000);
  // Out of the 5-minute window an hour later; still in the hour.
  EXPECT_TRUE(s.stats(kT0 + 30 * kMin, 5, "acme", "").empty());
  EXPECT_EQ(s.stats(kT0 + 30 * kMin, 60, "acme", "").size(), 2u);
  EXPECT_TRUE(s.stats(kT0 + 70 * kMin, 60, "acme", "").empty());
  EXPECT_EQ(s.stats(kT0, 5, "acme", "/b").size(), 1u);
  EXPECT_TRUE(s.stats(kT0, 5, "other", "").empty());
}

TEST(LiveStats, FlowsAndEviction) {
  LiveStats s(3, 2);
  s.record(kT0, "acme", "/b", "", "", 200, 10, "/a");
  s.record(kT0, "acme", "/b", "", "", 200, 10, "/a");
  s.record(kT0, "acme", "/c", "", "", 200, 10, "/b");
  auto f = s.flows(kT0, 5, "acme", "/b");
  ASSERT_EQ(f.size(), 2u);
  EXPECT_EQ(f[0].from, "/a");
  EXPECT_EQ(f[0].n, 2);
  for (int i = 0; i < 5; ++i) s.record(kT0, "acme", "/p" + std::to_string(i), "", "", 200, 10, "");
  EXPECT_EQ(s.paths(), 3u);  // the least recently seen went first
  s.record(kT0, "acme", "/x", "", "", 200, 10, "/y");
  EXPECT_LE(s.flows(kT0, 5, "acme", "").size(), 2u);
}
