// Heat studies (design/machine/048.HeatStudy.md): endpoint keys, caps, per-client minutes,
// concurrency, suggestions, the limits on studies, ending, recovery, and pruning.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <thread>

#include "goober/base/files.h"
#include "goober/json/json.h"
#include "goober/operations/heat.h"

using namespace goober::operations;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kT0 = 1793232000000;
constexpr int64_t kMin = 60000;

// A request owning the strings a HeatSample views; it converts to one for record().
struct Req {
  HeatSample base;
  std::string box, method, what, client;
  operator HeatSample() const {
    HeatSample s = base;
    s.box = box;
    s.method = method;
    s.what = what;
    s.client = client;
    return s;
  }
};

// "GET /p" on box shop by default; "content" and "mcp" for those APIs; "(not found)" a group.
Req sample(int64_t at_ms, const std::string& endpoint, uint64_t client, int64_t us = 1000,
           const std::string& api = "box:shop", int status = 200) {
  Req r;
  r.base.at_ms = at_ms;
  r.base.end_us = at_ms * 1000;  // a monotonic clock that agrees with the wall clock, for tests
  r.base.us = us;
  r.base.status = status;
  r.base.bytes_out = 100;
  r.base.api = api.starts_with("box:") ? HeatApi::kBox : api == "mcp" ? HeatApi::kMcp : HeatApi::kContent;
  if (api.starts_with("box:")) r.box = api.substr(4);
  auto space = endpoint.find(' ');
  if (space == std::string::npos || endpoint.starts_with("(")) {
    r.what = endpoint;
  } else {
    for (char c : endpoint.substr(0, space)) r.method.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    r.what = endpoint.substr(space + 1);
  }
  r.client = "198.51.100." + std::to_string(client);
  return r;
}

fs::path fresh(const std::string& name) {
  auto dir = fs::path(GOOBER_TEST_TMP) / ("heat_" + name);
  fs::remove_all(dir);
  return dir;
}

const bj::object* find_endpoint(const bj::object& rep, const std::string& endpoint) {
  for (const auto& e : rep.at("endpoints").as_array()) {
    if (e.at("endpoint").as_string() == endpoint) return &e.as_object();
  }
  return nullptr;
}

}  // namespace

TEST(HeatStudy, EndpointMatchFromDeclaredPatterns) {
  EXPECT_EQ(endpoint_match("get", "/posts/{id : int64}"), "GET /posts/{id}");
  EXPECT_EQ(endpoint_match("post", "/a/{x}/b/{y:string}"), "POST /a/{x}/b/{y}");
  EXPECT_EQ(endpoint_match("delete", "/"), "DELETE /");
  EXPECT_EQ(endpoint_match("get", ""), "GET /");
  EXPECT_EQ(endpoint_match("get", "/files/{rest*}"), "GET /files/{rest*}");
}

// An endpoint's name is built from its parts once; each API names its own way, and a HEAD the
// host folded to get is its GET.
TEST(HeatStudy, EndpointsAreNamedByTheirAPI) {
  HeatStudies hs(fresh("names"));
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
  hs.record("a.example.com", sample(kT0 + 1, "GET /posts/{id : int64}", 1));
  hs.record("a.example.com", sample(kT0 + 2, "GET /about", 1, 1000, "content"));
  hs.record("a.example.com", sample(kT0 + 3, "search", 1, 1000, "mcp"));
  hs.record("a.example.com", sample(kT0 + 4, "(not found)", 1, 1000, "content", 404));
  auto rep = *hs.get(1, kT0 + 10, &err);
  ASSERT_TRUE(find_endpoint(rep, "GET /posts/{id}")) << goober::json::dump(rep);
  EXPECT_EQ(find_endpoint(rep, "GET /posts/{id}")->at("api").as_string(), "box:shop");
  EXPECT_EQ(find_endpoint(rep, "GET /about")->at("api").as_string(), "content");
  EXPECT_EQ(find_endpoint(rep, "search")->at("api").as_string(), "mcp");
  EXPECT_EQ(find_endpoint(rep, "(not found)")->at("status").at("4xx").as_int64(), 1);
  EXPECT_EQ(rep.at("endpoints").as_array().size(), 4u);
}

TEST(HeatStudy, CountHistogramIsExactBelow64AndCloseAbove) {
  CountHistogram h;
  for (uint32_t v = 1; v <= 50; ++v) h.add(v);
  EXPECT_EQ(h.percentile(0.5), 25u);
  EXPECT_EQ(h.percentile(1.0), 50u);
  CountHistogram big;
  big.add(1000);
  uint32_t p = big.percentile(0.99);
  EXPECT_EQ(p, 1000u);  // clamped to the largest seen
  big.add(5000);
  big.add(5000);
  EXPECT_GE(big.percentile(0.5), 5000u * 7 / 8);
  EXPECT_LE(big.percentile(0.5), 5000u);
  for (int b = 0; b < CountHistogram::kBuckets - 1; ++b) {
    EXPECT_LT(CountHistogram::upper(b), CountHistogram::upper(b + 1)) << b;
    EXPECT_EQ(CountHistogram::bucket(CountHistogram::upper(b)), b) << b;
  }
}

TEST(HeatStudy, DistinctCounterIsCloseEnough) {
  DistinctCounter d;
  for (uint64_t i = 0; i < 20000; ++i) d.add(i * 7919);
  int64_t e = d.estimate();
  EXPECT_GT(e, 20000 * 85 / 100);
  EXPECT_LT(e, 20000 * 115 / 100);
  DistinctCounter small;
  for (uint64_t i = 0; i < 10; ++i) small.add(i);
  EXPECT_NEAR(double(small.estimate()), 10.0, 1.0);
}

TEST(HeatStudy, OnePerDomainFourHostWideAndMinutesBounded) {
  HeatStudies hs(fresh("limits"));
  std::string err;
  EXPECT_FALSE(hs.start("a.example.com", "a", 4, "", "op", kT0, &err));
  EXPECT_NE(err.find("5 to 1440"), std::string::npos);
  EXPECT_FALSE(hs.start("a.example.com", "a", 1441, "", "op", kT0, &err));
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "busy hour", "op", kT0, &err)) << err;
  EXPECT_FALSE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
  EXPECT_NE(err.find("one per domain"), std::string::npos) << err;
  ASSERT_TRUE(hs.start("b.example.com", "b", 60, "", "op", kT0, &err));
  ASSERT_TRUE(hs.start("c.example.com", "c", 60, "", "op", kT0, &err));
  ASSERT_TRUE(hs.start("d.example.com", "d", 60, "", "op", kT0, &err));
  EXPECT_FALSE(hs.start("e.example.com", "e", 60, "", "op", kT0, &err));
  EXPECT_NE(err.find("4 heat studies"), std::string::npos) << err;
  EXPECT_EQ(hs.running(), 4);
  EXPECT_TRUE(hs.watching("a.example.com"));
  EXPECT_FALSE(hs.watching("e.example.com"));
  // Stopping one makes room.
  ASSERT_TRUE(hs.stop(1, "op", kT0 + kMin, &err)) << err;
  EXPECT_TRUE(hs.start("e.example.com", "e", 60, "", "op", kT0 + kMin, &err)) << err;
}

TEST(HeatStudy, NothingIsWatchedWhenNothingRuns) {
  HeatStudies hs(fresh("idle"));
  EXPECT_FALSE(hs.watching("a.example.com"));
  hs.record("a.example.com", sample(kT0, "GET /", 1));  // ignored, and cheap
  EXPECT_EQ(hs.list("", kT0).size(), 0u);
}

TEST(HeatStudy, CapFoldsTheRestIntoOtherAndPromotesBusyOnes) {
  HeatStudies::Caps caps;
  caps.endpoints = 2;
  HeatStudies hs(fresh("cap"), caps);
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
  hs.record("a.example.com", sample(kT0 + 1, "GET /one", 1));
  hs.record("a.example.com", sample(kT0 + 2, "GET /two", 1));
  for (int i = 0; i < 10; ++i) hs.record("a.example.com", sample(kT0 + 3 + i, "GET /three", 1));
  hs.record("a.example.com", sample(kT0 + 20, "GET /four", 1));
  auto rep = *hs.get(1, kT0 + 30, &err);
  EXPECT_EQ(rep.at("tracked").at("endpoints").as_int64(), 2);
  EXPECT_EQ(rep.at("tracked").at("other_requests").as_int64(), 11);
  ASSERT_TRUE(find_endpoint(rep, "other"));
  EXPECT_FALSE(find_endpoint(rep, "GET /three"));
  // A minute on, /three is busier than the least busy tracked endpoint and takes its place.
  hs.tick(kT0 + kMin + 1);
  for (int i = 0; i < 5; ++i) hs.record("a.example.com", sample(kT0 + kMin + 2 + i, "GET /three", 1));
  rep = *hs.get(1, kT0 + kMin + 10, &err);
  const auto* three = find_endpoint(rep, "GET /three");
  ASSERT_TRUE(three) << goober::json::dump(rep);
  EXPECT_EQ(three->at("requests").as_int64(), 5);
  EXPECT_EQ(three->at("before_tracked").as_int64(), 10);
  EXPECT_EQ(rep.at("tracked").at("promoted").as_int64(), 1);
  EXPECT_EQ(rep.at("totals").at("requests").as_int64(), 18);
}

TEST(HeatStudy, EachClientsBusiestMinute) {
  HeatStudies hs(fresh("busiest"));
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
  // 99 clients ask twice in one minute and once in the next; one client asks 40 times in one minute.
  for (uint64_t c = 1; c <= 99; ++c) {
    hs.record("a.example.com", sample(kT0 + int64_t(c), "GET /p", c));
    hs.record("a.example.com", sample(kT0 + 100 + int64_t(c), "GET /p", c));
    hs.record("a.example.com", sample(kT0 + kMin + int64_t(c), "GET /p", c));
  }
  for (int i = 0; i < 40; ++i) hs.record("a.example.com", sample(kT0 + 2 * kMin + i, "GET /p", 1000));
  auto rep = *hs.get(1, kT0 + 3 * kMin, &err);
  const auto* p = find_endpoint(rep, "GET /p");
  ASSERT_TRUE(p);
  const auto& c = p->at("clients").as_object();
  EXPECT_EQ(c.at("distinct").as_int64(), 100);
  EXPECT_TRUE(c.at("exact").as_bool());
  EXPECT_EQ(c.at("busiest_minute").at("p50").as_int64(), 2);
  EXPECT_EQ(c.at("busiest_minute").at("p99").as_int64(), 2);
  EXPECT_EQ(c.at("busiest_minute").at("max").as_int64(), 40);
  EXPECT_EQ(p->at("per_minute").at("peak").as_int64(), 198);
  // The domain's busiest clients by keyed hash only.
  const auto& top = rep.at("clients").at("top").as_array();
  ASSERT_FALSE(top.empty());
  EXPECT_EQ(top[0].at("requests").as_int64(), 40);
  EXPECT_TRUE(std::regex_match(std::string(top[0].at("client_h").as_string()), std::regex("[0-9a-f]{16}")));
  EXPECT_EQ(top[0].at("endpoint").as_string(), "GET /p");
  EXPECT_NEAR(double(rep.at("clients").at("distinct").as_int64()), 100.0, 7.0);
}

TEST(HeatStudy, ClientsPastTheCapAreFoldedAndCountedApproximately) {
  HeatStudies::Caps caps;
  caps.endpoint_clients = 4;
  HeatStudies hs(fresh("clientcap"), caps);
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
  // Minute 0: 4 clients fill the table; a fifth in the same minute isn't attributed.
  for (uint64_t c = 1; c <= 5; ++c) hs.record("a.example.com", sample(kT0 + int64_t(c), "GET /p", c));
  // Minute 1: the idle ones are folded, so new clients fit again.
  for (uint64_t c = 6; c <= 9; ++c) hs.record("a.example.com", sample(kT0 + kMin + int64_t(c), "GET /p", c));
  auto rep = *hs.get(1, kT0 + 2 * kMin, &err);
  const auto& c = find_endpoint(rep, "GET /p")->at("clients").as_object();
  EXPECT_FALSE(c.at("exact").as_bool());
  EXPECT_EQ(c.at("unattributed_requests").as_int64(), 1);
  EXPECT_NEAR(double(c.at("distinct").as_int64()), 9.0, 1.0);
  EXPECT_EQ(c.at("busiest_minute").at("max").as_int64(), 1);
}

TEST(HeatStudy, PeakConcurrencyFromFinishedRequests) {
  HeatStudies hs(fresh("concurrent"));
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
  auto at = [&](int64_t start_ms, int64_t end_ms, uint64_t client) {
    auto s = sample(kT0 + end_ms, "POST /slow", client, (end_ms - start_ms) * 1000);
    s.base.end_us = (kT0 + end_ms) * 1000;
    hs.record("a.example.com", s);
  };
  // A long request [0, 1000) finishing last, three short ones inside it (two by client 7, at
  // once), and one after: four at once at 300 ms; client 7 had two at once.
  at(200, 400, 7);
  at(300, 500, 7);
  at(300, 350, 8);
  at(0, 1000, 9);
  at(2000, 2100, 7);
  auto rep = *hs.get(1, kT0 + kMin, &err);
  const auto& c = find_endpoint(rep, "POST /slow")->at("concurrent").as_object();
  EXPECT_EQ(c.at("peak").as_int64(), 4);
  EXPECT_EQ(c.at("per_client_peak").as_int64(), 2);
}

TEST(HeatStudy, SuggestionFormulas) {
  SuggestInput in;
  in.study_id = 3;
  in.domain = "a.example.com";
  in.api = "box:shop";
  in.endpoint = "POST /cart/{id}";
  in.requests = 500;
  in.clients = 40;
  in.p99_client_minute = 12;
  in.peak_minute = 90;
  in.peak_concurrent = 3;
  in.peak_client_concurrent = 1;
  in.heat_share = 0.1;
  in.top_client_share = 0.3;
  in.max_body_bytes = 3000;
  in.max_us = 450000;
  auto s = suggest_controls(in);
  const auto& a = s.at("control_set").as_object();
  EXPECT_EQ(a.at("mode").as_string(), "observe");
  EXPECT_EQ(a.at("domain").as_string(), "a.example.com");
  EXPECT_EQ(a.at("api").as_string(), "box:shop");
  EXPECT_EQ(a.at("endpoint").at("match").as_string(), "POST /cart/{id}");
  EXPECT_EQ(a.at("endpoint").at("id").as_string(), "post_cart_id");
  EXPECT_EQ(a.at("set").at("rate").at("per-minute").as_int64(), 24);  // max(10, 2 x 12)
  EXPECT_EQ(a.at("set").at("rate").at("burst").as_int64(), 12);
  EXPECT_EQ(a.at("set").at("rate").at("by").as_string(), "client");
  EXPECT_EQ(a.at("set").at("concurrent").at("max").as_int64(), 6);  // max(2, 2 x 3)
  EXPECT_EQ(a.at("set").at("concurrent").at("per-client").as_int64(), 2);
  EXPECT_EQ(a.at("set").at("body-max-bytes").as_int64(), 6144);  // 2 x 3000, rounded up to a KiB
  EXPECT_EQ(a.at("set").at("timeout-ms").as_int64(), 1000);      // max(1000, 2 x 450 ms)
  EXPECT_NE(std::string(s.at("reasons").at("rate").as_string()).find("p99"), std::string::npos);
  // Floors.
  in.p99_client_minute = 2;
  in.peak_concurrent = 0;
  in.peak_client_concurrent = 0;
  in.max_body_bytes = 0;
  in.max_us = 4'000'000;
  s = suggest_controls(in);
  EXPECT_EQ(s.at("control_set").at("set").at("rate").at("per-minute").as_int64(), 10);
  EXPECT_EQ(s.at("control_set").at("set").at("rate").at("burst").as_int64(), 5);
  EXPECT_EQ(s.at("control_set").at("set").at("concurrent").at("max").as_int64(), 2);
  EXPECT_EQ(s.at("control_set").at("set").at("concurrent").at("per-client").as_int64(), 1);
  EXPECT_FALSE(s.at("control_set").at("set").as_object().contains("body-max-bytes"));
  EXPECT_EQ(s.at("control_set").at("set").at("timeout-ms").as_int64(), 8000);
  // Heat spread over many clients: the whole endpoint's rate is the risk.
  in.heat_share = 0.4;
  in.top_client_share = 0.02;
  s = suggest_controls(in);
  EXPECT_EQ(s.at("control_set").at("set").at("rate").at("by").as_string(), "all");
  EXPECT_EQ(s.at("control_set").at("set").at("rate").at("per-minute").as_int64(), 180);  // 2 x 90
  // Content gets no timeout; too few requests or a grouped row gets no suggestion.
  in.api = "content";
  in.endpoint = "GET /about";
  EXPECT_FALSE(suggest_controls(in).at("control_set").at("set").as_object().contains("timeout-ms"));
  in.requests = 19;
  EXPECT_TRUE(suggest_controls(in).contains("none"));
  in.requests = 500;
  in.api = "mcp";
  in.endpoint = "search";
  {
    // One MCP tool, matched as "TOOL /<name>" (047): rate and concurrent, no timeout.
    auto tool = suggest_controls(in);
    ASSERT_TRUE(tool.contains("control_set")) << goober::json::dump(tool);
    EXPECT_FALSE(tool.at("control_set").as_object().contains("api"));  // TOOL takes none (047 § 13)
    EXPECT_EQ(tool.at("control_set").at("endpoint").at("match").as_string(), "TOOL /search");
    EXPECT_EQ(tool.at("control_set").at("endpoint").at("id").as_string(), "tool_search");
    EXPECT_TRUE(tool.at("control_set").at("set").as_object().contains("rate"));
    EXPECT_FALSE(tool.at("control_set").at("set").as_object().contains("timeout-ms"));
    in.endpoint = "(rpc)";
    EXPECT_TRUE(suggest_controls(in).contains("none"));
  }
  in.api = "content";
  in.requests = 500;
  in.endpoint = "(not found)";
  EXPECT_TRUE(suggest_controls(in).contains("none"));
  in.endpoint = "other";
  in.api = "other";
  EXPECT_TRUE(suggest_controls(in).contains("none"));
}

TEST(HeatStudy, StopKeepsWhatWasGathered) {
  auto dir = fresh("stop");
  HeatStudies hs(dir);
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "why", "op", kT0, &err));
  for (int i = 0; i < 30; ++i) hs.record("a.example.com", sample(kT0 + i * 1000, "GET /x", uint64_t(i % 3)));
  auto row = hs.stop(1, "grok", kT0 + 2 * kMin, &err);
  ASSERT_TRUE(row) << err;
  EXPECT_EQ(row->at("state").as_string(), "stopped");
  EXPECT_EQ(row->at("stopped_by").as_string(), "grok");
  EXPECT_FALSE(hs.watching("a.example.com"));
  EXPECT_FALSE(hs.stop(1, "grok", kT0 + 3 * kMin, &err));
  EXPECT_NE(err.find("already ended"), std::string::npos);
  auto rep = *hs.get(1, kT0 + 3 * kMin, &err);
  EXPECT_EQ(rep.at("totals").at("requests").as_int64(), 30);
  EXPECT_EQ(rep.at("ended_by").as_string(), "operator");
  EXPECT_TRUE(find_endpoint(rep, "GET /x")->at("suggest").as_object().contains("control_set"));
  EXPECT_TRUE(fs::exists(dir / "1.json"));
  EXPECT_LE(fs::file_size(dir / "1.json"), HeatStudies::kReportMaxBytes);
  // Requests after the end aren't counted.
  hs.record("a.example.com", sample(kT0 + 4 * kMin, "GET /x", 1));
  EXPECT_EQ(hs.get(1, kT0 + 5 * kMin, &err)->at("totals").at("requests").as_int64(), 30);
}

// A minute's save built just before a stop never lands after the final report (code review C5):
// the file says "stopped", so a restart doesn't find it "running" and call it interrupted.
TEST(HeatStudy, ASaveRacingAStopNeverOverwritesTheFinalReport) {
  auto dir = fresh("save_race");
  HeatStudies hs(dir);
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "why", "op", kT0, &err));
  hs.record("a.example.com", sample(kT0 + 1000, "GET /x", 1));
  bool stopped = false;
  hs.set_before_save_for_test([&](int64_t id) { stopped = hs.stop(id, "grok", kT0 + 2 * kMin, &err).has_value(); });
  EXPECT_TRUE(hs.tick(kT0 + 2 * kMin).empty());
  ASSERT_TRUE(stopped) << err;
  auto text = goober::base::read_file(dir / "1.json");
  ASSERT_TRUE(text);
  auto v = goober::json::parse(*text, &err);
  ASSERT_TRUE(v) << err;
  EXPECT_EQ(v->at("state").as_string(), "stopped");
  hs.set_before_save_for_test({});
  HeatStudies again(dir);
  EXPECT_EQ(again.get(1, kT0 + 3 * kMin, &err)->at("state").as_string(), "stopped");
}

// A name a visitor chose (an MCP tool's, a path) is kept to 200 bytes, cut on a character.
TEST(HeatStudy, EndpointNamesAreCappedAt200Bytes) {
  HeatStudies hs(fresh("cap"));
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
  std::string name = std::string(199, 'x') + "\xc3\xa9" + std::string(100, 'y');  // é across byte 200
  hs.record("a.example.com", sample(kT0 + 1000, name, 1, 1000, "mcp"));
  auto rep = *hs.get(1, kT0 + kMin, &err);
  ASSERT_EQ(rep.at("endpoints").as_array().size(), 1u);
  EXPECT_EQ(rep.at("endpoints").at(0).at("endpoint").as_string(), std::string(199, 'x'));
}

TEST(HeatStudy, EndsWhenTimeIsUp) {
  HeatStudies hs(fresh("timeup"));
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 5, "", "op", kT0, &err));
  hs.record("a.example.com", sample(kT0 + kMin, "GET /x", 1));
  EXPECT_TRUE(hs.tick(kT0 + 4 * kMin).empty());
  hs.record("a.example.com", sample(kT0 + 5 * kMin, "GET /x", 1));  // at the end: not counted
  auto ended = hs.tick(kT0 + 5 * kMin + 500);
  ASSERT_EQ(ended.size(), 1u);
  EXPECT_EQ(ended[0].at("state").as_string(), "done");
  EXPECT_EQ(ended[0].at("ended_ms").as_int64(), kT0 + 5 * kMin);
  EXPECT_EQ(ended[0].at("requests").as_int64(), 1);
  EXPECT_EQ(hs.running(), 0);
}

TEST(HeatStudy, ARestartKeepsTheLastSave) {
  auto dir = fresh("restart");
  {
    HeatStudies hs(dir);
    std::string err;
    ASSERT_TRUE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
    for (int i = 0; i < 25; ++i) hs.record("a.example.com", sample(kT0 + i, "GET /x", 1));
    hs.tick(kT0 + kMin);                                                // saved
    hs.record("a.example.com", sample(kT0 + kMin + 5, "GET /x", 1));  // after the save: lost
    // No stop: the process dies.
  }
  HeatStudies again(dir);
  ASSERT_EQ(again.recovered().size(), 1u);
  std::string err;
  auto rep = *again.get(1, kT0 + 2 * kMin, &err);
  EXPECT_EQ(rep.at("state").as_string(), "interrupted");
  EXPECT_EQ(rep.at("ended_by").as_string(), "restart");
  EXPECT_EQ(rep.at("ended_ms").as_int64(), kT0 + kMin);
  EXPECT_EQ(rep.at("totals").at("requests").as_int64(), 25);
  EXPECT_FALSE(again.watching("a.example.com"));
  // New ids carry on past the old ones.
  ASSERT_TRUE(again.start("a.example.com", "a", 60, "", "op", kT0 + 3 * kMin, &err));
  EXPECT_EQ(again.list("", kT0 + 3 * kMin).at(0).at("id").as_int64(), 2);
}

TEST(HeatStudy, StopAllWhenTheHostStops) {
  HeatStudies hs(fresh("stopall"));
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 60, "", "op", kT0, &err));
  ASSERT_TRUE(hs.start("b.example.com", "b", 60, "", "op", kT0, &err));
  auto ended = hs.stop_all(kT0 + kMin);
  ASSERT_EQ(ended.size(), 2u);
  EXPECT_EQ(ended[0].at("ended_by").as_string(), "host_stopping");
  EXPECT_EQ(hs.running(), 0);
}

TEST(HeatStudy, ReportsAreKept30DaysAndWithinTheBudget) {
  auto dir = fresh("prune");
  HeatStudies hs(dir);
  std::string err;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(hs.start("a.example.com", "a", 5, "", "op", kT0 + i * kMin, &err)) << err;
    hs.stop(i + 1, "op", kT0 + i * kMin + 1000, &err);
  }
  EXPECT_EQ(hs.list("", kT0).size(), 3u);
  EXPECT_EQ(hs.prune(kT0 + HeatStudies::kKeepMs - kMin, 0), 0);
  // The first ended at kT0 + 1 s: past 30 days it goes.
  EXPECT_EQ(hs.prune(kT0 + 1000 + HeatStudies::kKeepMs, 0), 1);
  EXPECT_FALSE(fs::exists(dir / "1.json"));
  EXPECT_TRUE(fs::exists(dir / "2.json"));
  // Over the budget, the oldest finished goes first.
  int64_t one = int64_t(fs::file_size(dir / "3.json"));
  EXPECT_EQ(hs.prune(kT0, one), 1);
  EXPECT_FALSE(fs::exists(dir / "2.json"));
  EXPECT_TRUE(fs::exists(dir / "3.json"));
  EXPECT_EQ(hs.bytes(), one);
  EXPECT_FALSE(hs.get(1, kT0, &err));
}

// The largest study: a day long, 600 endpoints (500 tracked), many clients. Its report stays
// within kReportMaxBytes and says how much memory it held.
TEST(HeatStudy, ADayOfManyEndpointsStaysSmall) {
  auto dir = fresh("big");
  HeatStudies hs(dir);
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 1440, "", "op", kT0, &err));
  for (int minute = 0; minute < 1440; minute += 7) {
    for (int e = 0; e < 600; e += (minute % 3) + 1) {
      hs.record("a.example.com", sample(kT0 + minute * kMin + e, "GET /page/" + std::to_string(e), uint64_t(e * 31 + minute)));
    }
  }
  auto running = *hs.get(1, kT0 + 1439 * kMin, &err);
  int64_t memory = running.at("memory_bytes").as_int64();
  EXPECT_GT(memory, 0);
  EXPECT_LT(memory, int64_t(16) << 20);
  ASSERT_TRUE(hs.stop(1, "op", kT0 + 1439 * kMin, &err)) << err;
  EXPECT_LE(fs::file_size(dir / "1.json"), HeatStudies::kReportMaxBytes);
  std::printf("heat study: a day of 600 endpoints held %.1f MiB; its report is %.0f KiB\n", double(memory) / (1 << 20),
              double(fs::file_size(dir / "1.json")) / 1024);
  auto rep = *hs.get(1, kT0 + 1440 * kMin, &err);
  EXPECT_EQ(rep.at("tracked").at("endpoints").as_int64(), 500);
  EXPECT_EQ(rep.at("endpoints").at(0).as_object().contains("suggest"), true);  // every detail fit
  EXPECT_GT(rep.at("tracked").at("other_requests").as_int64(), 0);
}

TEST(HeatStudy, ListFiltersByDomainAndNamesTheTenant) {
  HeatStudies hs(fresh("list"));
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "acme", 60, "", "op", kT0, &err));
  ASSERT_TRUE(hs.start("b.example.com", "other", 60, "", "op", kT0, &err));
  EXPECT_EQ(hs.list("", kT0).size(), 2u);
  auto only = hs.list("a.example.com", kT0);
  ASSERT_EQ(only.size(), 1u);
  EXPECT_EQ(only[0].at("tenant").as_string(), "acme");
  EXPECT_EQ(*hs.tenant_of(2), "other");
  EXPECT_FALSE(hs.tenant_of(9));
}

// The cost of the request path's hook (048 § 3.1): a realistic mix of endpoints and clients fed
// to a running study, and the check on a domain nothing studies. Printed; bounded loosely so a
// slow build doesn't fail it.
TEST(HeatStudy, RecordCost) {
  HeatStudies hs(fresh("cost"));
  std::string err;
  ASSERT_TRUE(hs.start("a.example.com", "a", 1440, "", "op", kT0, &err));
  std::vector<Req> mix;
  for (int i = 0; i < 4096; ++i) {
    mix.push_back(sample(kT0, "GET /e/" + std::to_string(i % 40) + "/{id}", uint64_t(i) * 2654435761u % 3000,
                         200 + (i % 7) * 100));
  }
  constexpr int kN = 400000;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    HeatSample s = mix[size_t(i) % mix.size()];
    s.at_ms = kT0 + int64_t(i) * 10;  // most of a day at 100 requests a second
    s.end_us = s.at_ms * 1000;
    if (hs.watching("a.example.com")) hs.record("a.example.com", s);
  }
  double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / kN;
  auto t1 = std::chrono::steady_clock::now();
  int watched = 0;
  for (int i = 0; i < kN; ++i) watched += hs.watching("b.example.com");
  double other_ns =
      double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t1).count()) / kN;
  // Four request threads at once, as the host's network and box pools are.
  auto t2 = std::chrono::steady_clock::now();
  {
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t] {
        for (int i = t; i < kN; i += 4) {
          HeatSample s = mix[size_t(i) % mix.size()];
          s.at_ms = kT0 + int64_t(i) * 10;
          s.end_us = s.at_ms * 1000;
          if (hs.watching("a.example.com")) hs.record("a.example.com", s);
        }
      });
    }
    for (auto& th : threads) th.join();
  }
  double four_ns =
      double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t2).count()) / kN;
  // Nothing running: one relaxed load.
  ASSERT_TRUE(hs.stop(1, "op", kT0 + kMin, &err)) << err;
  auto t3 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) watched += hs.watching("a.example.com");
  double idle_ns =
      double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t3).count()) / kN;
  std::printf("heat study: %.0f ns a request recorded; %.0f ns a request from four threads at once; %.1f ns to check an "
              "unstudied domain; %.1f ns when nothing runs\n",
              ns, four_ns, other_ns, idle_ns);
  EXPECT_EQ(watched, 0);
  EXPECT_LT(ns, 100000);
}
