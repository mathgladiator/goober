// Control reports (design/machine/047.Controls.md § 7): series, totals, the effect of the last
// change, and the verdicts.

#include <gtest/gtest.h>

#include "goober/controls/gate.h"
#include "goober/controls/report.h"

using namespace goober;
using namespace goober::controls;

namespace {

constexpr int64_t kNow = 1'800'000'000'000;
constexpr int64_t kHour = 3'600'000;

operations::ControlRow row(int64_t t, int64_t matched, int64_t refused = 0, int clients = 0, const std::string& top = "",
                           int64_t top_n = 0) {
  operations::ControlRow r;
  r.t = t / kBucketMs * kBucketMs;
  r.domain = "example.com";
  r.control = "c";
  r.matched = matched;
  r.allowed = matched - refused;
  r.rate = refused;
  for (int i = 0; i < clients; ++i) r.clients = sketch_add(r.clients, "client-" + std::to_string(i));
  r.top_client = top;
  r.top_n = top_n;
  for (int64_t i = 0; i < r.allowed; ++i) r.hist.add(1000);
  return r;
}

std::vector<std::string> verdicts(const bj::object& r) {
  std::vector<std::string> out;
  for (const auto& v : r.at("verdicts").as_array()) out.emplace_back(v.as_string());
  return out;
}

bool says(const bj::object& r, std::string_view text) {
  for (const auto& v : verdicts(r)) {
    if (v.find(text) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST(ControlsReport, NeverMatched) {
  ReportInput in{"c", "endpoint", Mode::kEnforce, {}, {}};
  auto r = report(in, "1h", kNow);
  EXPECT_TRUE(says(r, "never matched in 1h"));
  EXPECT_EQ(r.at("points").as_array().size(), 12u);
  EXPECT_TRUE(r.at("effect").is_null());
}

TEST(ControlsReport, LooseWhenItNeverRefuses) {
  ReportInput in{"c", "endpoint", Mode::kEnforce, {row(kNow - 10 * 60'000, 50)}, {}};
  auto r = report(in, "1h", kNow);
  EXPECT_TRUE(says(r, "matched 50 but never refused (loose or not needed)"));
  EXPECT_EQ(r.at("totals").at("matched"), 50);
}

TEST(ControlsReport, ShieldingWhenOneClientHoldsTheRefusals) {
  ReportInput in{"c", "endpoint", Mode::kEnforce,
                 {row(kNow - 20 * 60'000, 100, 40, 2, "aa", 30), row(kNow - 10 * 60'000, 100, 20, 2, "aa", 18)}, {}};
  auto r = report(in, "1h", kNow);
  EXPECT_TRUE(says(r, "refusals come mostly from one client (80% of 60): shielding"));
  EXPECT_FALSE(says(r, "too tight"));
  EXPECT_NEAR(r.at("totals").at("top_client_share").as_double(), 0.8, 1e-9);
}

TEST(ControlsReport, TooTightWhenManyClientsAreRefused) {
  ReportInput in{"c", "api", Mode::kEnforce, {row(kNow - 10 * 60'000, 100, 30, 25, "bb", 2)}, {}};
  auto r = report(in, "1h", kNow);
  EXPECT_TRUE(says(r, "refuses many clients"));
  EXPECT_TRUE(says(r, "may be too tight"));
  EXPECT_GE(r.at("totals").at("clients_refused").as_int64(), 20);
}

TEST(ControlsReport, ObserveSaysWhatItWouldHaveRefused) {
  auto r1 = row(kNow - 10 * 60'000, 200);
  r1.would = 20;
  for (int i = 0; i < 4; ++i) r1.would_clients = sketch_add(r1.would_clients, "w" + std::to_string(i));
  ReportInput in{"c", "endpoint", Mode::kObserve, {r1}, {}};
  auto r = report(in, "12h", kNow);
  EXPECT_TRUE(says(r, "observe: would have refused 10% (20 of 200) from 4 clients")) << json::dump(r.at("verdicts"));
  ReportInput quiet{"c", "endpoint", Mode::kObserve, {row(kNow - 10 * 60'000, 5)}, {}};
  EXPECT_TRUE(says(report(quiet, "12h", kNow), "observe: would have refused nothing"));
}

// The effect compares equal spans before and after the last change.
TEST(ControlsReport, EffectComparesEqualSpansAroundTheLastChange) {
  int64_t changed = kNow - 2 * kHour;
  std::vector<operations::ControlRow> rows;
  rows.push_back(row(changed - 90 * 60'000, 100, 0));  // before
  rows.push_back(row(changed - 3 * kHour, 999, 0));    // too long before: outside the span
  auto bad = row(changed - 30 * 60'000, 100, 0);
  bad.n5 = 10;
  rows.push_back(bad);
  rows.push_back(row(changed + 30 * 60'000, 120, 30));  // after
  Change c{changed, "token:ops", "tighten the login rate", bj::object{{"mode", "enforce"}}, bj::object{{"mode", "enforce"}}};
  ReportInput in{"c", "endpoint", Mode::kEnforce, rows, {c}};
  auto r = report(in, "12h", kNow);
  const auto& e = r.at("effect").as_object();
  EXPECT_EQ(e.at("span_ms"), 2 * kHour);
  EXPECT_EQ(e.at("before").at("requests"), 200);
  EXPECT_EQ(e.at("before").at("refused"), 0);
  EXPECT_NEAR(e.at("before").at("ratio_5xx").as_double(), 0.05, 1e-9);
  EXPECT_EQ(e.at("after").at("requests"), 120);
  EXPECT_EQ(e.at("after").at("refused"), 30);
  EXPECT_EQ(r.at("last_change").at("why"), "tighten the login rate");
  // A control created at the change says nothing was counted under it before.
  Change created{changed, "token:ops", "new", nullptr, bj::object{{"mode", "observe"}}};
  ReportInput fresh{"c", "endpoint", Mode::kObserve, rows, {created}};
  EXPECT_NE(json::dump(report(fresh, "12h", kNow).at("effect")).find("nothing was counted under it before"), std::string::npos);
}

TEST(ControlsReport, AFewRefusalsAreTooFewToJudge) {
  ReportInput in{"c", "endpoint", Mode::kEnforce, {row(kNow - 10 * 60'000, 3, 2, 1, "aa", 2)}, {}};
  auto r = report(in, "1h", kNow);
  EXPECT_TRUE(says(r, "refused 2 of 3 (67%) from 1 client: too few to judge yet")) << json::dump(r.at("verdicts"));
  EXPECT_FALSE(says(r, "shielding"));
}

TEST(ControlsReport, FiveHundredsBehindAControlAreSaid) {
  auto r1 = row(kNow - 10 * 60'000, 100);
  r1.n5 = 10;
  ReportInput in{"c", "api", Mode::kEnforce, {r1}, {}};
  EXPECT_TRUE(says(report(in, "1h", kNow), "10% of the allowed answered 5xx"));
}

TEST(ControlsReport, WindowsAreTheTrafficOnes) {
  EXPECT_TRUE(valid_report_window("1h"));
  EXPECT_TRUE(valid_report_window("12h"));
  EXPECT_TRUE(valid_report_window("7d"));
  EXPECT_FALSE(valid_report_window("24h"));
  ReportInput in{"c", "api", Mode::kEnforce, {}, {}};
  auto r = report(in, "7d", kNow);
  EXPECT_EQ(r.at("points").as_array().size(), 168u);
  EXPECT_EQ(r.at("step_ms"), kHour);
}
