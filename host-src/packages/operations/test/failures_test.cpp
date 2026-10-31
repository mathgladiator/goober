#include <gtest/gtest.h>

#include "goober/operations/failures.h"

using namespace goober;
using operations::Failures;
namespace fs = std::filesystem;

namespace {

// 2026-10-29T12:00:00Z
constexpr int64_t kNoon = 1793275200000;

fs::path fresh(const char* name) {
  auto p = fs::path(GOOBER_TEST_TMP) / name;
  fs::remove_all(p);
  return p / "failures.db";
}

Failures::Event event(int64_t at, const std::string& route, const std::string& cls, const std::string& message,
                      int64_t n = 1) {
  Failures::Event e;
  e.at_ms = at;
  e.tenant = "example";
  e.box = "places";
  e.version = "3";
  e.route = route;
  e.kind = "get";
  e.cls = cls;
  e.message = message;
  e.request_id = "r1";
  e.n = n;
  return e;
}

}  // namespace

TEST(Failures, CallsAndFailuresAreCountedPerRouteAndClass) {
  Failures f(fresh("failures-count"));
  for (int i = 0; i < 10; ++i) f.call(kNoon + i, "example", "places", "list", "get");
  for (int i = 0; i < 4; ++i) f.call(kNoon + i, "example", "places", "nightly", "timer");
  f.failure(event(kNoon, "list", "trap", "trap: unreachable"));
  f.failure(event(kNoon + 1, "list", "timeout", "timeout: 2000 ms"));
  f.failure(event(kNoon + 2, "list", "sql_error", "no such column: nme", 2));
  f.failure(event(kNoon + 3, "list", "no_clock", "box SQL has no clock: date('now') is refused"));
  auto t = event(kNoon + 4, "nightly", "commit_failed", "commit failed: database is locked");
  t.kind = "timer";
  f.failure(t);
  f.flush(kNoon + 10);
  auto r = f.report("1h", "", "", kNoon + 60000);
  const auto& totals = r.at("totals").as_object();
  EXPECT_EQ(totals.at("calls").as_int64(), 14);
  EXPECT_EQ(totals.at("failures").as_int64(), 3);  // trap, timeout, commit_failed: the calls that failed
  EXPECT_EQ(totals.at("classes").at("sql_error").as_int64(), 2);
  const auto& routes = r.at("routes").as_array();
  ASSERT_EQ(routes.size(), 2u);
  EXPECT_EQ(routes[0].at("route"), "list");
  EXPECT_EQ(routes[0].at("calls").as_int64(), 10);
  EXPECT_EQ(routes[0].at("failures").as_int64(), 2);
  EXPECT_EQ(routes[0].at("classes").at("no_clock").as_int64(), 1);
  EXPECT_EQ(routes[1].at("kind"), "timer");
  const auto& latest = r.at("latest").as_array();
  ASSERT_EQ(latest.size(), 5u);
  EXPECT_EQ(latest[0].at("class"), "commit_failed");  // newest first
  EXPECT_EQ(r.at("groups").at("refused_write"), "sql");
  // The series has the calls and the failures in their minute.
  int64_t calls = 0, failures = 0;
  for (const auto& p : r.at("points").as_array()) {
    calls += p.at("calls").as_int64();
    failures += p.at("failures").as_int64();
  }
  EXPECT_EQ(calls, 14);
  EXPECT_EQ(failures, 3);
  // A box filter, and the hours behind the 7-day window.
  EXPECT_EQ(f.report("7d", "example", "places", kNoon + 60000).at("totals").at("calls").as_int64(), 14);
  EXPECT_EQ(f.report("1h", "", "other", kNoon + 60000).at("totals").at("calls").as_int64(), 0);
}

TEST(Failures, MessagesAreClippedAndOnlyTheNewestKept) {
  Failures f(fresh("failures-keep"));
  f.failure(event(kNoon, "list", "failed", std::string(1000, 'x')));
  for (int i = 0; i < 250; ++i) f.failure(event(kNoon + 1 + i, "list", "failed", "n" + std::to_string(i)));
  f.flush(kNoon + 1000);
  auto r = f.report("1h", "", "", kNoon + 2000, 1000);
  const auto& latest = r.at("latest").as_array();
  EXPECT_EQ(latest.size(), 200u);
  EXPECT_EQ(latest[0].at("message"), "n249");
  EXPECT_EQ(r.at("totals").at("failures").as_int64(), 251);  // counts keep every one
  Failures g(fresh("failures-clip"));
  g.failure(event(kNoon, "list", "failed", std::string(1000, 'x')));
  g.flush(kNoon);
  auto one = g.report("1h", "", "", kNoon);
  EXPECT_LT(one.at("latest").at(0).at("message").as_string().size(), 310u);
}

TEST(Failures, OldRowsAndMessagesAreDropped) {
  Failures f(fresh("failures-drop"));
  f.call(kNoon, "example", "places", "list", "get");
  f.failure(event(kNoon, "list", "trap", "trap"));
  f.flush(kNoon);
  int64_t later = kNoon + 40LL * 24 * 3600 * 1000;
  f.flush(later);
  auto r = f.report("30d", "", "", later);
  EXPECT_EQ(r.at("totals").at("calls").as_int64(), 0);
  EXPECT_TRUE(r.at("latest").as_array().empty());
  EXPECT_EQ(Failures::group_of("delivery_dead"), "outbox");
  EXPECT_EQ(Failures::group_of("nonsense"), "");
}
