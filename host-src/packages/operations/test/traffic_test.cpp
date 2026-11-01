#include <gtest/gtest.h>

#include <fstream>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/operations/traffic.h"

using namespace goober;
using operations::Histogram;
using operations::Traffic;
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

// 2026-10-29T12:00:00Z
constexpr int64_t kNoon = 1793275200000;

std::string at(int64_t ms) { return base::format_utc_ms(ms); }

std::optional<std::string> tenant_of(const std::string& domain) {
  if (domain == "acme.example") return std::string("acme");
  return std::nullopt;
}

class TrafficTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "traffic";
    fs::remove_all(root);
    fs::create_directories(root / "logs");
  }
  void line(const std::string& json) { base::append_line(root / "logs" / "access.log", json); }
  std::unique_ptr<Traffic> open() { return std::make_unique<Traffic>(root / "traffic.db", root / "logs", tenant_of); }
  fs::path root;
};

}  // namespace

TEST(Histogram, BucketsArePercentilesToWithinOne) {
  EXPECT_EQ(Histogram::bucket(0), 0);
  EXPECT_EQ(Histogram::bucket(32), 0);
  EXPECT_EQ(Histogram::bucket(33), 1);
  EXPECT_EQ(Histogram::upper(1), 45);
  EXPECT_EQ(Histogram::upper(2), 64);
  EXPECT_EQ(Histogram::bucket(int64_t(1) << 40), Histogram::kBuckets - 1);
  Histogram h;
  for (int i = 0; i < 98; ++i) h.add(1000);
  h.add(50000);
  h.add(50000);
  EXPECT_EQ(h.percentile(0.5), Histogram::upper(Histogram::bucket(1000)));
  EXPECT_GE(h.percentile(0.5), 1000);
  EXPECT_LT(h.percentile(0.5), 1415);
  EXPECT_GE(h.percentile(0.99), 50000);
  auto back = Histogram::decode(h.encode());
  EXPECT_EQ(back.counts, h.counts);
  EXPECT_EQ(Histogram().percentile(0.5), 0);
}

TEST(ParseUtc, ReadsTheAccessLogsTime) {
  EXPECT_EQ(operations::parse_utc_ms("2026-10-29T12:00:00.000Z"), kNoon);
  EXPECT_EQ(operations::parse_utc_ms(at(kNoon + 1234)), kNoon + 1234);
  EXPECT_FALSE(operations::parse_utc_ms("yesterday"));
}

TEST(KeyLine, HostWorkIsNamedAndNoPathIsKept) {
  auto key = [](const std::string& json) {
    std::string err;
    auto v = json::parse(json, &err);
    return *operations::key_line(v->as_object(), tenant_of);
  };
  auto t = at(kNoon);
  auto box = key(R"({"at":")" + t + R"(","domain":"acme.example","path":"/places/42","box":"acme","route":"place"})");
  EXPECT_EQ(box.tenant, "acme");
  EXPECT_EQ(box.box, "acme");
  EXPECT_EQ(box.route, "place");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"acme.example","path":"/about"})").route, "page");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"acme.example","path":"/identity/sign-in"})").route, "identity");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"acme.example","path":"/.well-known/x"})").route, "oauth");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"evil.test","path":"/wp-login.php"})").route, "unknown");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"ops.acme.example","path":"/api/x","panel":"x"})").route, "panel");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"x","path":"/goober-operations-mcp","operations":"mcp"})").route,
            "operations");
  // A timer's line names its tenant itself.
  auto timer = key(R"({"at":")" + t + R"(","domain":"127.0.0.1","path":"timer:t","box":"b","route":"t","tenant":"meat"})");
  EXPECT_EQ(timer.tenant, "meat");
}

TEST_F(TrafficTest, LinesAreSummedIntoSeriesAndRoutes) {
  for (int i = 0; i < 10; ++i) {
    line(R"({"at":")" + at(kNoon + i * 1000) + R"(","domain":"acme.example","path":"/p","status":200,"us":1000,"box":"acme","route":"place","fuel":10,"sql":2})");
  }
  line(R"({"at":")" + at(kNoon + 90 * 1000) + R"(","domain":"acme.example","path":"/p","status":500,"us":90000,"box":"acme","route":"place","fuel":5,"sql":1})");
  line(R"({"at":")" + at(kNoon + 90 * 1000) + R"(","domain":"acme.example","path":"/missing","status":404,"us":100})");
  auto t = open();
  EXPECT_EQ(t->catch_up(kNoon + 120 * 1000), 12u);
  auto routes = t->routes("1h", "", "", kNoon + 120 * 1000);
  ASSERT_EQ(routes.size(), 2u);
  const auto& place = routes[0].as_object();
  EXPECT_EQ(place.at("route"), "place");
  EXPECT_EQ(place.at("n").as_int64(), 11);
  EXPECT_EQ(place.at("n5").as_int64(), 1);
  EXPECT_EQ(place.at("fuel").as_int64(), 105);
  EXPECT_EQ(place.at("sql").as_int64(), 21);
  EXPECT_EQ(place.at("us_max").as_int64(), 90000);
  EXPECT_LT(place.at("p50_us").as_int64(), 1415);
  EXPECT_GE(place.at("p99_us").as_int64(), 90000);
  EXPECT_EQ(routes[1].as_object().at("route"), "page");
  EXPECT_EQ(routes[1].as_object().at("n4").as_int64(), 1);

  auto s = t->series("1h", "acme", "acme", kNoon + 120 * 1000);
  const auto& points = s.at("points").as_array();
  ASSERT_EQ(points.size(), 60u);
  int64_t n = 0;
  for (const auto& p : points) n += p.as_object().at("n").as_int64();
  EXPECT_EQ(n, 11);
  // The noon minute holds ten, the next one the failure.
  int64_t from = s.at("from_ms").as_int64();
  EXPECT_EQ(points[size_t((kNoon - from) / 60000)].as_object().at("n").as_int64(), 10);
  EXPECT_EQ(points[size_t((kNoon + 60000 - from) / 60000)].as_object().at("n5").as_int64(), 1);

  // Hours were written alongside: the 7-day window sees the same requests.
  auto week = t->routes("7d", "acme", "", kNoon + 120 * 1000);
  ASSERT_EQ(week.size(), 2u);
  EXPECT_EQ(week[0].as_object().at("n").as_int64(), 11);

  auto hot = t->hot_boxes("12h", kNoon + 120 * 1000);
  ASSERT_EQ(hot.size(), 1u);
  EXPECT_EQ(hot[0].as_object().at("box"), "acme");
  EXPECT_DOUBLE_EQ(hot[0].as_object().at("share").as_double(), 1.0);
}

TEST_F(TrafficTest, ARestartResumesAtTheCursorAndARotationIsFollowed) {
  line(R"({"at":")" + at(kNoon) + R"(","domain":"acme.example","path":"/","status":200,"us":10})");
  {
    auto t = open();
    EXPECT_EQ(t->catch_up(kNoon), 1u);
    EXPECT_EQ(t->catch_up(kNoon), 0u);
  }
  line(R"({"at":")" + at(kNoon + 1) + R"(","domain":"acme.example","path":"/","status":200,"us":10})");
  // A rotation, as append_line does it, then more lines in the new file.
  fs::rename(root / "logs" / "access.log", root / "logs" / "access.log.1");
  line(R"({"at":")" + at(kNoon + 2) + R"(","domain":"acme.example","path":"/","status":200,"us":10})");
  // A line still being written (no newline yet) waits for the next pass.
  {
    std::ofstream out(root / "logs" / "access.log", std::ios::app);
    out << R"({"at":")" << at(kNoon + 3) << R"(","domain")";
  }
  auto t = open();
  EXPECT_EQ(t->catch_up(kNoon), 2u);
  {
    std::ofstream out(root / "logs" / "access.log", std::ios::app);
    out << R"(:"acme.example","path":"/","status":200,"us":10})" << "\n";
  }
  EXPECT_EQ(t->catch_up(kNoon), 1u);
  auto routes = t->routes("1h", "", "", kNoon);
  ASSERT_EQ(routes.size(), 1u);
  EXPECT_EQ(routes[0].as_object().at("n").as_int64(), 4);
}

TEST_F(TrafficTest, EdgesComeFromInternalCallsAndTheEgressLog) {
  line(R"({"at":")" + at(kNoon) + R"(","domain":"acme.example","path":"internal:geo/x","status":200,"us":50,"box":"geo","route":"x","internal":"acme -> geo"})");
  base::append_line(root / "logs" / "egress.log",
                    R"({"at":")" + at(kNoon) + R"(","tenant":"acme","box":"acme","service":"xai","status":200,"us":900000,"bytes_out":300,"bytes_in":2000})");
  base::append_line(root / "logs" / "egress.log",
                    R"({"at":")" + at(kNoon) + R"(","tenant":"acme","box":"acme","service":"xai","status":0,"us":10,"bytes_out":300,"bytes_in":0,"error":"timeout"})");
  auto t = open();
  EXPECT_EQ(t->catch_up(kNoon), 3u);
  auto edges = t->edges("1h", "acme", kNoon);
  ASSERT_EQ(edges.size(), 2u);
  const auto& svc = edges[0].as_object();
  EXPECT_EQ(svc.at("to_kind"), "service");
  EXPECT_EQ(svc.at("to_name"), "xai");
  EXPECT_EQ(svc.at("n").as_int64(), 2);
  EXPECT_EQ(svc.at("errors").as_int64(), 1);
  EXPECT_EQ(svc.at("bytes_in").as_int64(), 2000);
  const auto& box = edges[1].as_object();
  EXPECT_EQ(box.at("to_kind"), "box");
  EXPECT_EQ(box.at("to_name"), "geo");
  // Asking for the callee finds the edge into it too.
  EXPECT_EQ(t->edges("1h", "geo", kNoon).size(), 1u);
}

TEST_F(TrafficTest, OldMinutesAreDroppedAndHoursKept) {
  line(R"({"at":")" + at(kNoon) + R"(","domain":"acme.example","path":"/","status":200,"us":10})");
  auto t = open();
  t->catch_up(kNoon);
  int64_t later = kNoon + 3 * 24 * 3600 * 1000LL;
  t->catch_up(later);
  int64_t n = 0;
  auto old = t->series("12h", "", "", kNoon);
  for (const auto& p : old.at("points").as_array()) n += p.as_object().at("n").as_int64();
  EXPECT_EQ(n, 0);
  EXPECT_EQ(t->routes("7d", "", "", later).size(), 1u);
}

// The Errors view (028 § 4.2): what visitors got, by who made it, with every path nothing
// answers counted as one place and box-to-box calls left out.
TEST_F(TrafficTest, ErrorsFromOutsideAreCountedByPlaceAndMaker) {
  auto L = [&](int64_t ms, const std::string& rest) {
    line(R"({"at":")" + at(ms) + R"(","domain":"acme.example",)" + rest + "}");
  };
  for (int i = 0; i < 6; ++i) L(kNoon + i * 1000, R"("path":"/","status":200,"us":10,"file":"index.md")");
  for (const char* scan : {"/wp-login.php", "/.env", "/admin.php"}) {
    L(kNoon, std::string(R"("path":")") + scan + R"(","status":404,"us":10,"error_by":"host")");
  }
  L(kNoon, R"("path":"/places/9","status":404,"us":10,"box":"acme","route":"place","outcome":"ok","error_by":"box","ref":"same")");
  L(kNoon, R"("path":"/places/8","status":500,"us":10,"box":"acme","route":"place","outcome":"trap","error_by":"box_failed","ref":"other")");
  L(kNoon, R"("path":"/members","status":401,"us":10,"file":"members.md","error_by":"host")");
  L(kNoon, R"("path":"internal:acme/x","status":500,"us":10,"box":"acme","route":"x","outcome":"trap","internal":"web -> acme")");
  auto t = open();
  t->catch_up(kNoon + 60000);
  auto e = t->errors("1h", "acme", kNoon + 60000);
  const auto& totals = e.at("totals").as_object();
  EXPECT_EQ(totals.at("n").as_int64(), 13);  // every request, the internal call's included
  EXPECT_EQ(totals.at("n4").as_int64(), 5);
  EXPECT_EQ(totals.at("n5").as_int64(), 1);  // the internal call's 500 isn't an outside error
  EXPECT_EQ(totals.at("host").as_int64(), 4);
  EXPECT_EQ(totals.at("box").as_int64(), 1);
  EXPECT_EQ(totals.at("box_failed").as_int64(), 1);
  const auto& places = e.at("places").as_array();
  ASSERT_EQ(places.size(), 4u);
  EXPECT_EQ(places[0].at("place"), "(not found)");
  EXPECT_EQ(places[0].at("n").as_int64(), 3);
  EXPECT_EQ(places[0].at("none").as_int64(), 3);
  std::string all = json::dump(e);
  EXPECT_EQ(all.find("wp-login"), std::string::npos) << all;
  EXPECT_NE(all.find(R"("place":"/members")"), std::string::npos) << all;
  EXPECT_NE(all.find(R"("route":"place","status":500,"error_by":"box_failed")"), std::string::npos) << all;
  EXPECT_EQ(e.at("referred").at("same").as_int64(), 1);
  EXPECT_EQ(e.at("referred").at("other").as_int64(), 1);
  EXPECT_EQ(e.at("referred").at("none").as_int64(), 4);
  ASSERT_EQ(e.at("statuses").as_array().size(), 3u);  // 401, 404, 500
  // The 7-day window reads the hours, written alongside.
  EXPECT_EQ(t->errors("7d", "", kNoon + 60000).at("totals").at("n4").as_int64(), 5);
}

// External services (028 § 4.4): every call out and every mail the mailer sent, by service and
// method, with failures by status and the latest ones kept with their request ids.
TEST_F(TrafficTest, ServicesAreCountedFromTheEgressAndMailLogs) {
  auto egress = [&](const std::string& json) { base::append_line(root / "logs" / "egress.log", json); };
  egress(R"({"at":")" + at(kNoon) + R"(","tenant":"acme","box":"shop","service":"payments","method":"charge","status":200,"us":1000,"bytes_out":10,"bytes_in":20,"request_id":"r1"})");
  egress(R"({"at":")" + at(kNoon) + R"(","tenant":"acme","box":"shop","service":"payments","method":"charge","status":503,"us":2000,"url":"https://pay.example/v1/charge","request_id":"r2"})");
  egress(R"({"at":")" + at(kNoon + 1000) + R"(","tenant":"acme","box":"shop","service":"payments","method":"refund","status":0,"us":30000000,"error":"timed out","outbox_id":7})");
  egress(R"({"at":")" + at(kNoon) + R"(","tenant":"acme","box":"","service":"push","method":"send","status":201,"us":500})");
  auto mail = [&](const std::string& json) { base::append_line(root / "logs" / "mail.log", json); };
  mail(R"({"at":")" + at(kNoon) + R"(","tenant":"acme","via":"ses","status":"sent","us":40000})");
  mail(R"({"at":")" + at(kNoon) + R"(","realm":"acme","via":"ses","status":"failed","error":"throttled"})");
  auto t = open();
  EXPECT_EQ(t->catch_up(kNoon + 2000), 6u);

  auto all = t->services("1h", "", "", kNoon + 2000);
  EXPECT_EQ(all.at("totals").at("calls"), 6);
  EXPECT_EQ(all.at("totals").at("failed"), 3);
  const auto& first = all.at("services").at(0).as_object();
  EXPECT_EQ(first.at("service"), "payments");
  EXPECT_EQ(first.at("calls"), 3);
  EXPECT_EQ(first.at("n5"), 1);
  EXPECT_EQ(first.at("neterr"), 1);
  EXPECT_EQ(first.at("outbox"), 1);
  EXPECT_GE(first.at("p99_us").as_int64(), 16000000);  // the last bucket: everything past about 16 s
  // The mailer: a box's mail is from "boxes", the host's own from "host".
  bool boxes = false, host = false;
  for (const auto& m : all.at("methods").as_array()) {
    const auto& o = m.as_object();
    if (o.at("service") != "mailer") continue;
    EXPECT_EQ(o.at("method"), "ses");
    if (o.at("box") == "boxes") boxes = o.at("failed") == 0;
    if (o.at("box") == "host") host = o.at("failed") == 1;
  }
  EXPECT_TRUE(boxes && host) << json::dump(all.at("methods"));

  // One service: its methods, statuses, and latest failures, newest first.
  auto pay = t->services("1h", "acme", "payments", kNoon + 2000);
  EXPECT_EQ(pay.at("methods").as_array().size(), 2u);
  EXPECT_EQ(pay.at("statuses").as_array().size(), 2u);
  const auto& latest = pay.at("latest").as_array();
  ASSERT_EQ(latest.size(), 2u);
  EXPECT_EQ(latest[0].at("method"), "refund");
  EXPECT_EQ(latest[0].at("outbox_id"), 7);
  EXPECT_EQ(latest[1].at("request_id"), "r2");
  EXPECT_EQ(latest[1].at("url"), "https://pay.example/v1/charge");
  // Counted once, across a restart.
  t.reset();
  auto again = open();
  again->catch_up(kNoon + 3000);
  EXPECT_EQ(again->services("1h", "", "", kNoon + 3000).at("totals").at("calls"), 6);
  // Hours keep it after minutes go.
  int64_t later = kNoon + 3 * 24 * 3600 * 1000LL;
  again->catch_up(later);
  EXPECT_EQ(again->services("7d", "", "payments", later).at("totals").at("calls"), 3);
}

TEST_F(TrafficTest, ServiceFailuresKeepTheNewestHundredPerService) {
  for (int i = 0; i < 130; ++i) {
    base::append_line(root / "logs" / "egress.log",
                      R"({"at":")" + at(kNoon + i) +
                          R"(","tenant":"acme","box":"b","service":"flaky","method":"m","status":500,"request_id":"r)" +
                          std::to_string(i) + R"("})");
  }
  auto t = open();
  t->catch_up(kNoon + 1000);
  auto r = t->services("1h", "", "flaky", kNoon + 1000, 500);
  EXPECT_EQ(r.at("totals").at("failed"), 130);
  EXPECT_EQ(r.at("latest").as_array().size(), 100u);
  EXPECT_EQ(r.at("latest").at(0).at("request_id"), "r129");
}

// Levels sampled over time (028 § 4.4): the outbox's pending elements per service.
TEST_F(TrafficTest, GaugesKeepEachMinutesLargestAndTheLatest) {
  auto t = open();
  t->gauge(kNoon, "outbox_pending", "acme", {{"payments", 4}, {"mail", 1}});
  t->gauge(kNoon + 2000, "outbox_pending", "acme", {{"payments", 2}});
  t->gauge(kNoon + 61000, "outbox_pending", "acme", {});
  t->catch_up(kNoon + 61000);
  auto g = t->gauge_series("1h", "outbox_pending", "", kNoon + 61000);
  int64_t peak = 0, nulls = 0;
  for (const auto& p : g.at("points").as_array()) {
    if (p.at("value").is_null()) ++nulls;
    else peak = std::max(peak, p.at("value").as_int64());
  }
  EXPECT_EQ(peak, 5);  // the minute's largest per key, summed: 4 + 1
  EXPECT_GT(nulls, 50);
  // payments read 0 at the last sample; mail had read 0 before and is forgotten.
  const auto& now = g.at("now").as_array();
  ASSERT_EQ(now.size(), 1u);
  EXPECT_EQ(now[0].at("key"), "payments");
  EXPECT_EQ(now[0].at("value"), 0);
  EXPECT_TRUE(t->gauge_series("1h", "outbox_pending", "other", kNoon + 61000).at("now").as_array().empty());
}
