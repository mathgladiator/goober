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
  if (domain == "kcmeatup.com") return std::string("kcmeatup");
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
  auto box = key(R"({"at":")" + t + R"(","domain":"kcmeatup.com","path":"/places/42","box":"kc","route":"place"})");
  EXPECT_EQ(box.tenant, "kcmeatup");
  EXPECT_EQ(box.box, "kc");
  EXPECT_EQ(box.route, "place");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"kcmeatup.com","path":"/about"})").route, "page");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"kcmeatup.com","path":"/identity/sign-in"})").route, "identity");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"kcmeatup.com","path":"/.well-known/x"})").route, "oauth");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"evil.test","path":"/wp-login.php"})").route, "unknown");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"ops.kcmeatup.com","path":"/api/x","panel":"x"})").route, "panel");
  EXPECT_EQ(key(R"({"at":")" + t + R"(","domain":"x","path":"/goober-operations-mcp","operations":"mcp"})").route,
            "operations");
  // A timer's line names its tenant itself.
  auto timer = key(R"({"at":")" + t + R"(","domain":"127.0.0.1","path":"timer:t","box":"b","route":"t","tenant":"meat"})");
  EXPECT_EQ(timer.tenant, "meat");
}

TEST_F(TrafficTest, LinesAreSummedIntoSeriesAndRoutes) {
  for (int i = 0; i < 10; ++i) {
    line(R"({"at":")" + at(kNoon + i * 1000) + R"(","domain":"kcmeatup.com","path":"/p","status":200,"us":1000,"box":"kc","route":"place","fuel":10,"sql":2})");
  }
  line(R"({"at":")" + at(kNoon + 90 * 1000) + R"(","domain":"kcmeatup.com","path":"/p","status":500,"us":90000,"box":"kc","route":"place","fuel":5,"sql":1})");
  line(R"({"at":")" + at(kNoon + 90 * 1000) + R"(","domain":"kcmeatup.com","path":"/missing","status":404,"us":100})");
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

  auto s = t->series("1h", "kcmeatup", "kc", kNoon + 120 * 1000);
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
  auto week = t->routes("7d", "kcmeatup", "", kNoon + 120 * 1000);
  ASSERT_EQ(week.size(), 2u);
  EXPECT_EQ(week[0].as_object().at("n").as_int64(), 11);

  auto hot = t->hot_boxes("12h", kNoon + 120 * 1000);
  ASSERT_EQ(hot.size(), 1u);
  EXPECT_EQ(hot[0].as_object().at("box"), "kc");
  EXPECT_DOUBLE_EQ(hot[0].as_object().at("share").as_double(), 1.0);
}

TEST_F(TrafficTest, ARestartResumesAtTheCursorAndARotationIsFollowed) {
  line(R"({"at":")" + at(kNoon) + R"(","domain":"kcmeatup.com","path":"/","status":200,"us":10})");
  {
    auto t = open();
    EXPECT_EQ(t->catch_up(kNoon), 1u);
    EXPECT_EQ(t->catch_up(kNoon), 0u);
  }
  line(R"({"at":")" + at(kNoon + 1) + R"(","domain":"kcmeatup.com","path":"/","status":200,"us":10})");
  // A rotation, as append_line does it, then more lines in the new file.
  fs::rename(root / "logs" / "access.log", root / "logs" / "access.log.1");
  line(R"({"at":")" + at(kNoon + 2) + R"(","domain":"kcmeatup.com","path":"/","status":200,"us":10})");
  // A line still being written (no newline yet) waits for the next pass.
  {
    std::ofstream out(root / "logs" / "access.log", std::ios::app);
    out << R"({"at":")" << at(kNoon + 3) << R"(","domain")";
  }
  auto t = open();
  EXPECT_EQ(t->catch_up(kNoon), 2u);
  {
    std::ofstream out(root / "logs" / "access.log", std::ios::app);
    out << R"(:"kcmeatup.com","path":"/","status":200,"us":10})" << "\n";
  }
  EXPECT_EQ(t->catch_up(kNoon), 1u);
  auto routes = t->routes("1h", "", "", kNoon);
  ASSERT_EQ(routes.size(), 1u);
  EXPECT_EQ(routes[0].as_object().at("n").as_int64(), 4);
}

TEST_F(TrafficTest, EdgesComeFromInternalCallsAndTheEgressLog) {
  line(R"({"at":")" + at(kNoon) + R"(","domain":"kcmeatup.com","path":"internal:geo/x","status":200,"us":50,"box":"geo","route":"x","internal":"kc -> geo"})");
  base::append_line(root / "logs" / "egress.log",
                    R"({"at":")" + at(kNoon) + R"(","tenant":"kcmeatup","box":"kc","service":"xai","status":200,"us":900000,"bytes_out":300,"bytes_in":2000})");
  base::append_line(root / "logs" / "egress.log",
                    R"({"at":")" + at(kNoon) + R"(","tenant":"kcmeatup","box":"kc","service":"xai","status":0,"us":10,"bytes_out":300,"bytes_in":0,"error":"timeout"})");
  auto t = open();
  EXPECT_EQ(t->catch_up(kNoon), 3u);
  auto edges = t->edges("1h", "kc", kNoon);
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
  line(R"({"at":")" + at(kNoon) + R"(","domain":"kcmeatup.com","path":"/","status":200,"us":10})");
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
