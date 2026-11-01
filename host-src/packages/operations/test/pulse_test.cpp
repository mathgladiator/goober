#include "goober/operations/pulse.h"

#include <gtest/gtest.h>

using goober::operations::Pulse;

TEST(Pulse, CountsTheLastFiveMinutes) {
  Pulse p;
  int64_t t = 1000 * Pulse::kMinuteMs;
  p.request(t, 200, 1000);
  p.request(t, 404, 2000);
  p.request(t + 1, 503, 50000);
  p.add(t, Pulse::kBoxCalls, 3);
  p.add(t, Pulse::kBoxFailed);
  auto s = p.snapshot(t + 2);
  EXPECT_EQ(s.n[Pulse::kRequests], 3);
  EXPECT_EQ(s.n[Pulse::kRequests4xx], 1);
  EXPECT_EQ(s.n[Pulse::kRequests5xx], 1);
  EXPECT_EQ(s.n[Pulse::kBoxCalls], 3);
  EXPECT_EQ(s.n[Pulse::kBoxFailed], 1);
  EXPECT_EQ(s.max_us, 50000);
  EXPECT_GE(s.p99_us, 50000);
  EXPECT_GE(s.p50_us, 2000);

  // Four minutes on, still counted; five minutes on, gone; a reused slot starts empty.
  EXPECT_EQ(p.snapshot(t + 4 * Pulse::kMinuteMs).n[Pulse::kRequests], 3);
  EXPECT_EQ(p.snapshot(t + 5 * Pulse::kMinuteMs).n[Pulse::kRequests], 0);
  p.request(t + 5 * Pulse::kMinuteMs, 200, 10);
  auto later = p.snapshot(t + 5 * Pulse::kMinuteMs);
  EXPECT_EQ(later.n[Pulse::kRequests], 1);
  EXPECT_EQ(later.n[Pulse::kRequests5xx], 0);
  EXPECT_EQ(later.max_us, 10);
}

TEST(Pulse, EmptyIsZero) {
  Pulse p;
  auto s = p.snapshot(123456789);
  EXPECT_EQ(s.n[Pulse::kRequests], 0);
  EXPECT_EQ(s.p99_us, 0);
}
