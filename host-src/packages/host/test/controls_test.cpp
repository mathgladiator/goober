// Per-domain controls through a real host (design/machine/047.Controls.md): observe then
// enforce, each refusal and its status, closed without waking a box, concurrency released on
// every finish path, timeout-ms, by: user, the tools' validation, host defaults, the change log,
// the audit log, the access log, and /goober/ready's ratios.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <regex>
#include <thread>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kToken = "controls-token-0123456789abcdef";
const std::vector<std::pair<std::string, std::string>> kSameSite = {{"origin", "http://example.com"}};

struct Reply {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};

Reply fetch(uint16_t port, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "example.com");
  for (const auto& [k, v] : headers) req.set(k, v);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  Reply r{int(res.result_int()), res.body(), {}};
  for (const auto& f : res) r.headers[base::to_lower(std::string(f.name_string()))] = std::string(f.value());
  return r;
}

const char* kWat = R"wat((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22ok\22:true}")
  (func (export "goober_route_items") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 11))) (i32.const 0))
  (func (export "goober_route_cart") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 11))) (i32.const 0))
  (func (export "goober_route_private") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 11))) (i32.const 0))
  (func (export "goober_route_boom") (result i32) unreachable)
  (func (export "goober_route_spin") (result i32) (loop $l (br $l)) (i32.const 0))
))wat";

const char* kManifest = R"({"box":"shop","domains":["example.com"],
    "behavior":{"timeout-ms":5000,"fuel":2000000000},
    "routes":{"http":{
      "/api/items":{"get":{"name":"items","public":true}},
      "/api/cart/{id}":{"post":{"name":"cart","public":true}},
      "/api/private":{"get":{"name":"private","requirements":[["member"]]}},
      "/api/boom":{"get":{"name":"boom","public":true}},
      "/api/spin":{"get":{"name":"spin","public":true}}}}})";

std::string config_text(const std::string& extra = "") {
  return R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"example.com": {"tenant": "example"}, "acme.example": {"tenant": "acme"},
                  "www.example.com": {"redirect-to": "example.com"}},
      "operations": {"token": ")" +
         std::string(kToken) + R"("},
      "threads": {"network": 1, "boxes": 4})" +
         extra + "}";
}

class ControlsTest : public ::testing::Test {
 protected:
  void SetUp() override { start(""); }
  void start(const std::string& extra, bool fresh = true) {
    root = fs::path(GOOBER_TEST_TMP) / "controls";
    if (fresh) fs::remove_all(root);
    std::string err;
    cfg = config::parse_config(config_text(extra), root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    if (fresh) ASSERT_TRUE(host->boxes().upload(kManifest, kWat, &err)) << err;
  }
  void TearDown() override { host.reset(); }

  // A tool's answer, or {"error": text}.
  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"},
                   {"id", 1},
                   {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", std::string("Bearer ") + kToken}, {"content-type", "application/json"}},
                   json::dump(req));
    std::string err;
    auto v = json::parse(r.body, &err);
    if (!v || !v->as_object().contains("result")) return bj::object{{"error", r.body}};
    const auto& res = v->at("result").as_object();
    std::string text(res.at("content").at(0).at("text").as_string());
    if (res.at("isError").as_bool()) return bj::object{{"error", text}};
    if (res.contains("structuredContent")) return res.at("structuredContent");
    return *json::parse(text, &err);
  }
  std::string error_of(const std::string& name, bj::object args) {
    auto v = tool(name, std::move(args));
    if (!v.is_object() || !v.as_object().contains("error")) return "(no error: " + json::dump(v) + ")";
    return std::string(v.at("error").as_string());
  }
  std::string sign_in(const std::string& email) {
    const std::vector<std::pair<std::string, std::string>> form = {
        {"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://example.com"}};
    fetch(port, bhttp::verb::post, "/identity/email/start", form, "email=" + email);
    host->drain_mail();
    std::string code;
    for (const auto& f : base::list_files(root / "storage/mail/outbox")) {
      std::smatch m;
      auto text = *base::read_file(root / "storage/mail/outbox" / f);
      if (text.find("To: " + email) != std::string::npos && std::regex_search(text, m, std::regex("is ([0-9]{6})"))) code = m[1];
    }
    auto v = fetch(port, bhttp::verb::post, "/identity/email/verify", form, "email=" + email + "&code=" + code);
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }
  std::string access_log() { return base::read_file(root / "storage" / "logs" / "access.log").value_or(""); }

  fs::path root;
  std::shared_ptr<const config::HostConfig> cfg;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

}  // namespace

// The way an operator is meant to use them: observe first, read the report, then enforce.
TEST_F(ControlsTest, ObserveThenReadTheReportThenEnforce) {
  auto set = tool("control_set", {{"domain", "example.com"},
                                  {"endpoint", bj::object{{"id", "cart"}, {"match", "POST /api/cart/{id}"}}},
                                  {"set", bj::object{{"rate", bj::object{{"per-minute", 6}, {"burst", 2}}}}},
                                  {"mode", "observe"},
                                  {"why", "carts are being hammered"}});
  ASSERT_FALSE(set.as_object().contains("error")) << json::dump(set);
  EXPECT_EQ(set.at("level"), "endpoint");
  EXPECT_EQ(set.at("effective").at("observed").at("rate").at("from"), "cart");
  for (int i = 0; i < 5; ++i) EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/7", kSameSite).status, 200) << i;
  auto report = tool("control_report", {{"domain", "example.com"}, {"id", "cart"}, {"window", "1h"}});
  ASSERT_FALSE(report.as_object().contains("error")) << json::dump(report);
  const auto& r = report.at("controls").at(0);
  EXPECT_EQ(r.at("totals").at("matched"), 5);
  EXPECT_EQ(r.at("totals").at("refused"), 0);
  EXPECT_EQ(r.at("totals").at("would"), 3);
  EXPECT_NE(json::dump(r.at("verdicts")).find("observe: would have refused 60% (3 of 5) from 1 client"), std::string::npos)
      << json::dump(r.at("verdicts"));
  EXPECT_EQ(r.at("last_change").at("why"), "carts are being hammered");

  auto enforce = tool("control_set", {{"domain", "example.com"},
                                      {"endpoint", bj::object{{"id", "cart"}}},
                                      {"mode", "enforce"},
                                      {"why", "it would have refused only the flood"}});
  ASSERT_FALSE(enforce.as_object().contains("error")) << json::dump(enforce);
  EXPECT_EQ(enforce.at("before").at("mode"), "observe");
  EXPECT_NE(json::dump(enforce.at("notes")).find("now enforced"), std::string::npos);
  // The enforced bucket is its own: two, then 429.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/7", kSameSite).status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/8", kSameSite).status, 200);
  auto refused = fetch(port, bhttp::verb::post, "/api/cart/9", {{"origin", "http://example.com"}, {"accept", "application/json"}});
  EXPECT_EQ(refused.status, 429);
  EXPECT_EQ(refused.headers["retry-after"], "10");
  EXPECT_NE(refused.body.find("\"status\":429"), std::string::npos) << refused.body;
  // Other paths aren't the override's.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);

  auto log = access_log();
  EXPECT_NE(log.find(R"("control":{"id":"cart","outcome":"refused","reason":"rate")"), std::string::npos);
  EXPECT_NE(log.find(R"("control":{"id":"cart","outcome":"allowed")"), std::string::npos);
  EXPECT_NE(log.find(R"("would":["cart:rate"])"), std::string::npos);
  auto audit = base::read_file(root / "storage" / "logs" / "audit.log").value_or("");
  EXPECT_NE(audit.find("\"control.set\""), std::string::npos);
  EXPECT_NE(audit.find("it would have refused only the flood"), std::string::npos);
  auto changes = tool("changes", {{"kind", "control"}});
  EXPECT_NE(json::dump(changes).find("example.com"), std::string::npos) << json::dump(changes);

  auto list = tool("control_list", {{"domain", "example.com"}});
  const auto& c = list.at(0).at("controls").at(0);
  EXPECT_EQ(c.at("id"), "cart");
  EXPECT_EQ(c.at("mode"), "enforce");
  EXPECT_EQ(c.at("match"), "POST /api/cart/{id}");
  EXPECT_EQ(c.at("changes"), 2);
  EXPECT_EQ(c.at("effective").at("enforced").at("rate").at("value").at("per-minute"), 6);
  EXPECT_EQ(c.at("in_flight"), 0);
  auto after = tool("control_report", {{"domain", "example.com"}, {"id", "cart"}, {"window", "1h"}});
  EXPECT_EQ(after.at("controls").at(0).at("totals").at("rate"), 1);

  // Kept across a restart, through the WAL.
  host.reset();
  start("", false);
  auto again = tool("control_list", {{"domain", "example.com"}});
  EXPECT_EQ(again.at(0).at("controls").at(0).at("id"), "cart");
  auto kept = tool("control_report", {{"domain", "example.com"}, {"id", "cart"}, {"window", "1h"}});
  EXPECT_EQ(kept.at("controls").at(0).at("totals").at("matched"), 8);  // the open bucket was kept at stop
}

// closed answers without waking the box; a more specific override lifts it for one endpoint.
TEST_F(ControlsTest, ClosedAnswersWithoutWakingTheBox) {
  auto set = tool("control_set", {{"domain", "example.com"},
                                  {"api", "box:shop"},
                                  {"set", bj::object{{"closed", bj::object{{"retry-after-s", 600}, {"message", "Back at noon."}}}}},
                                  {"mode", "enforce"},
                                  {"why", "moving the data"}});
  ASSERT_FALSE(set.as_object().contains("error")) << json::dump(set);
  auto page = fetch(port, bhttp::verb::get, "/api/items", {{"accept", "text/html"}});
  EXPECT_EQ(page.status, 503);
  EXPECT_EQ(page.headers["retry-after"], "600");
  EXPECT_NE(page.body.find("Back at noon."), std::string::npos) << page.body;
  auto api = fetch(port, bhttp::verb::get, "/api/items", {{"accept", "application/json"}});
  EXPECT_EQ(api.status, 503);
  EXPECT_NE(api.body.find(R"("message":"Back at noon.")"), std::string::npos) << api.body;
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/nothing-here").status, 404);  // content isn't the box's API
  // No box ran: the access line has no outcome and the host made the error.
  auto log = access_log();
  std::smatch m;
  ASSERT_TRUE(std::regex_search(log, m, std::regex(R"(\{[^\n]*"path":"/api/items"[^\n]*\})")));
  EXPECT_EQ(m.str().find("\"outcome\":\"ok\""), std::string::npos);
  EXPECT_NE(m.str().find("\"error_by\":\"host\""), std::string::npos);

  auto lift = tool("control_set", {{"domain", "example.com"},
                                   {"endpoint", bj::object{{"id", "items_open"}, {"match", "GET /api/items"}}},
                                   {"set", bj::object{{"closed", "off"}}},
                                   {"mode", "enforce"},
                                   {"why", "the catalog stays up"}});
  ASSERT_FALSE(lift.as_object().contains("error")) << json::dump(lift);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/boom").status, 503);  // still closed
  auto removed = tool("control_remove", {{"domain", "example.com"}, {"id", "box:shop"}, {"why", "moved"}});
  ASSERT_FALSE(removed.as_object().contains("error")) << json::dump(removed);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/boom").status, 500);  // the box runs again, and traps
  auto report = tool("control_report", {{"domain", "example.com"}, {"window", "1h"}});
  std::string text = json::dump(report);
  EXPECT_NE(text.find("\"id\":\"box:shop\""), std::string::npos);  // removed, its rows still there
  EXPECT_NE(text.find("removed: its rows stay"), std::string::npos);
  EXPECT_NE(text.find("closed: answered 3 requests"), std::string::npos) << text;
}

// A slot is given back whatever way a request ends: success, a box that traps, a gate's 403, a
// cross-origin refusal, a page that isn't there, a host page.
TEST_F(ControlsTest, ConcurrencyIsReleasedOnEveryFinishPath) {
  auto set = tool("control_set", {{"domain", "example.com"},
                                  {"set", bj::object{{"concurrent", bj::object{{"max", 1}}}}},
                                  {"mode", "enforce"},
                                  {"why", "one at a time"}});
  ASSERT_FALSE(set.as_object().contains("error")) << json::dump(set);
  for (int round = 0; round < 2; ++round) {
    EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);
    EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/boom").status, 500);
    EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/private", {{"accept", "application/json"}}).status, 401);
    EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/1", {{"origin", "http://elsewhere.example"}}).status, 403);
    EXPECT_EQ(fetch(port, bhttp::verb::get, "/missing").status, 404);
    EXPECT_EQ(fetch(port, bhttp::verb::get, "/identity/sign-in").status, 200);
  }
  auto list = tool("control_list", {{"domain", "example.com"}});
  EXPECT_EQ(list.at(0).at("controls").at(0).at("in_flight"), 0);
  auto report = tool("control_report", {{"domain", "example.com"}, {"id", "defaults"}, {"window", "1h"}});
  const auto& t = report.at("controls").at(0).at("totals");
  EXPECT_EQ(t.at("matched"), 12);
  EXPECT_EQ(t.at("concurrent"), 0);
  EXPECT_EQ(t.at("peak"), 1);
  EXPECT_GT(t.at("ratio_5xx").as_double(), 0.0);
}

TEST_F(ControlsTest, BodyAndTimeoutOnlyLowerTheHosts) {
  auto body = tool("control_set", {{"domain", "example.com"},
                                   {"endpoint", bj::object{{"id", "small_carts"}, {"match", "POST /api/cart/{id}"}}},
                                   {"set", bj::object{{"body-max-bytes", 10}}},
                                   {"mode", "enforce"},
                                   {"why", "carts are tiny"}});
  ASSERT_FALSE(body.as_object().contains("error")) << json::dump(body);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/1", kSameSite, "0123456789").status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/1", kSameSite, "0123456789X").status, 413);
  EXPECT_NE(error_of("control_set", {{"domain", "example.com"},
                                     {"set", bj::object{{"body-max-bytes", int64_t(1) << 40}}},
                                     {"mode", "enforce"},
                                     {"why", "x"}}),
            "")
      << "larger than the host's";

  auto spin = tool("control_set", {{"domain", "example.com"},
                                   {"endpoint", bj::object{{"id", "spin_short"}, {"match", "GET /api/spin"}}},
                                   {"set", bj::object{{"timeout-ms", 100}}},
                                   {"mode", "enforce"},
                                   {"why", "never wait long for it"}});
  ASSERT_FALSE(spin.as_object().contains("error")) << json::dump(spin);
  auto t0 = std::chrono::steady_clock::now();
  auto r = fetch(port, bhttp::verb::get, "/api/spin");
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_GE(r.status, 500);
  EXPECT_LT(ms, 2000) << "the box's own 5000 ms would have run";
}

TEST_F(ControlsTest, ByUserCountsTheAccountNotTheAddress) {
  auto ann = sign_in("ann@example.com");
  auto bo = sign_in("bo@example.com");
  ASSERT_FALSE(ann.empty());
  ASSERT_FALSE(bo.empty());
  auto set = tool("control_set", {{"domain", "example.com"},
                                  {"endpoint", bj::object{{"id", "per_person"}, {"match", "GET /api/items"}}},
                                  {"set", bj::object{{"rate", bj::object{{"per-minute", 1}, {"burst", 1}, {"by", "user"}}}}},
                                  {"mode", "enforce"},
                                  {"why", "one each"}});
  ASSERT_FALSE(set.as_object().contains("error")) << json::dump(set);
  // The first request of each session counts as the address while its account is looked up.
  fetch(port, bhttp::verb::get, "/api/items", {{"cookie", ann}});
  fetch(port, bhttp::verb::get, "/api/items", {{"cookie", bo}});
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  // Same address, two people: one each.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items", {{"cookie", ann}}).status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items", {{"cookie", ann}}).status, 429);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items", {{"cookie", bo}}).status, 200);
  // Nobody signed in: the address's bucket, which the first two used.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 429);
}

// by: user knows a person by their session cookie alone: other cookies that change from request to
// request (an analytics id) don't make them a stranger counted as the address.
TEST_F(ControlsTest, ByUserKnowsThePersonWhateverOtherCookiesTheySend) {
  auto ann = sign_in("ann@example.com");
  ASSERT_FALSE(ann.empty());
  auto set = tool("control_set", {{"domain", "example.com"},
                                  {"endpoint", bj::object{{"id", "per_person"}, {"match", "GET /api/items"}}},
                                  {"set", bj::object{{"rate", bj::object{{"per-minute", 1}, {"burst", 1}, {"by", "user"}}}}},
                                  {"mode", "enforce"},
                                  {"why", "one each"}});
  ASSERT_FALSE(set.as_object().contains("error")) << json::dump(set);
  // The first request counts as the address (its one) while the account is looked up.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items", {{"cookie", "visit=1; " + ann}}).status, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  // Another analytics cookie, the same session: ann's own bucket, not the spent address's.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items", {{"cookie", "visit=2; " + ann}}).status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items", {{"cookie", ann + "; visit=3"}}).status, 429);
  // Cookies without a session are nobody: the address's bucket, spent.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items", {{"cookie", "visit=4"}}).status, 429);
}

TEST_F(ControlsTest, TheToolsRefuseWhatTheyCantKeep) {
  bj::object rate{{"rate", bj::object{{"per-minute", 60}}}};
  auto base = [&](bj::object extra) {
    bj::object a{{"domain", "example.com"}, {"mode", "observe"}, {"why", "testing"}, {"set", rate}};
    for (auto& kv : extra) a[kv.key()] = kv.value();
    return a;
  };
  EXPECT_NE(error_of("control_set", base({{"set", bj::object{{"ratee", 1}}}})).find("unknown key ratee"), std::string::npos);
  EXPECT_NE(error_of("control_set", base({{"set", bj::object{{"rate", bj::object{{"per-minute", -1}}}}}})).find("0.1 to 1000000"),
            std::string::npos);
  EXPECT_NE(error_of("control_set", base({{"endpoint", bj::object{{"id", "x"}, {"match", "get /a"}}}})).find("uppercase"),
            std::string::npos);
  EXPECT_NE(error_of("control_set", base({{"endpoint", bj::object{{"id", "x"}}}})).find("match is required"), std::string::npos);
  EXPECT_NE(error_of("control_set", base({{"endpoint", bj::object{{"id", "has-hyphen"}, {"match", "GET /a"}}}})).find("endpoint.id"),
            std::string::npos);
  EXPECT_NE(error_of("control_set", base({{"api", "box:"}})).find("box:<box name>"), std::string::npos);
  EXPECT_NE(error_of("control_set", base({{"domain", "www.example.com"}})).find("redirects to example.com"), std::string::npos);
  EXPECT_NE(error_of("control_set", base({{"domain", "nowhere.example"}})).find("no domain"), std::string::npos);
  EXPECT_NE(error_of("control_set", {{"domain", "example.com"}, {"set", rate}, {"why", "x"}}).find("mode is required"),
            std::string::npos);
  EXPECT_NE(error_of("control_set", {{"domain", "example.com"}, {"set", rate}, {"mode", "observe"}}).find("why"), std::string::npos);
  EXPECT_NE(error_of("control_set", base({{"mode", "sometimes"}})).find("enforce or observe"), std::string::npos);
  EXPECT_NE(error_of("control_remove", {{"domain", "example.com"}, {"id", "nope"}}).find("no control nope"), std::string::npos);
  EXPECT_NE(error_of("control_report", {{"domain", "example.com"}, {"window", "24h"}}).find("1h, 12h, or 7d"), std::string::npos);
  // A note, not a refusal, for a box that doesn't serve the domain yet.
  auto later = tool("control_set", base({{"api", "box:later"}}));
  EXPECT_NE(json::dump(later).find("no live box later serves example.com yet"), std::string::npos) << json::dump(later);
  // Nothing refused was stored.
  EXPECT_EQ(tool("control_list", {{"domain", "example.com"}}).at(0).at("controls").as_array().size(), 1u);
}

// host.json's controls.defaults apply under every domain's own, and /goober/ready says, as ratios
// with no domain named, how much the controls refused.
TEST_F(ControlsTest, HostDefaultsApplyEverywhereAndReadySaysTheRatio) {
  host.reset();
  start(R"(, "controls": {"defaults": {"rate": {"per-minute": 60, "burst": 2}}})");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 429);
  auto list = tool("control_list", {{"domain", "example.com"}});
  EXPECT_EQ(list.at(0).at("host_defaults").at("set").at("rate").at("per-minute"), 60);
  EXPECT_EQ(list.at(0).at("controls").at(0).at("level"), "host");
  auto report = tool("control_report", {{"domain", "example.com"}, {"window", "1h"}});
  EXPECT_EQ(report.at("controls").at(0).at("id"), "defaults");
  EXPECT_EQ(report.at("controls").at(0).at("totals").at("rate"), 1);
  auto ready = fetch(port, bhttp::verb::get, "/goober/ready");
  std::string err;
  auto v = json::parse(ready.body, &err);
  ASSERT_TRUE(v) << ready.body;
  EXPECT_GT(v->at("health").at("controls").at("refused_ratio").as_double(), 0.0);
  EXPECT_EQ(ready.body.find("example.com"), std::string::npos);
  auto prom = fetch(port, bhttp::verb::get, "/goober/ready?format=prometheus");
  EXPECT_NE(prom.body.find("goober_control_refused_ratio"), std::string::npos);
  // A domain's own defaults lift the host's rate for it alone.
  auto lift = tool("control_set", {{"domain", "example.com"},
                                   {"set", bj::object{{"rate", "off"}}},
                                   {"mode", "enforce"},
                                   {"why", "this one is fine"}});
  ASSERT_FALSE(lift.as_object().contains("error")) << json::dump(lift);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);
}

namespace {

// The human, 2026-11-03: a refused client reads "rate limit reached" and nothing that names the
// control, its level, or its bucket.
void expect_says_only_rate_limit_reached(const Reply& r, const std::vector<std::string>& never) {
  EXPECT_NE(r.body.find("rate limit reached"), std::string::npos) << r.body;
  for (const auto& word : never) EXPECT_EQ(r.body.find(word), std::string::npos) << word << " in " << r.body;
  for (const auto& [k, v] : r.headers) {
    for (const auto& word : never) EXPECT_EQ(v.find(word), std::string::npos) << word << " in header " << k;
  }
}

}  // namespace

TEST_F(ControlsTest, RefusalsSayOnlyRateLimitReached) {
  ASSERT_FALSE(tool("control_set", {{"domain", "example.com"},
                                    {"endpoint", bj::object{{"id", "secret_rule_name"}, {"match", "GET /api/items"}}},
                                    {"set", bj::object{{"rate", bj::object{{"per-minute", 1}, {"burst", 1}}}}},
                                    {"mode", "enforce"},
                                    {"why", "x"}})
                   .as_object()
                   .contains("error"));
  ASSERT_FALSE(tool("control_set", {{"domain", "example.com"},
                                    {"api", "box:shop"},
                                    {"set", bj::object{{"concurrent", bj::object{{"max", 1}}}}},
                                    {"mode", "enforce"},
                                    {"why", "x"}})
                   .as_object()
                   .contains("error"));
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);
  std::vector<std::string> never = {"secret_rule_name", "box:shop", "endpoint", "\"rate\"", "bucket", "control"};
  auto html = fetch(port, bhttp::verb::get, "/api/items", {{"accept", "text/html"}});
  EXPECT_EQ(html.status, 429);
  EXPECT_FALSE(html.headers["retry-after"].empty());
  expect_says_only_rate_limit_reached(html, never);
  auto api = fetch(port, bhttp::verb::get, "/api/items", {{"accept", "application/json"}});
  EXPECT_EQ(api.status, 429);
  EXPECT_NE(api.body.find(R"("error":"rate limit reached")"), std::string::npos) << api.body;
  expect_says_only_rate_limit_reached(api, never);

  // Concurrency: 503 with Retry-After: 1, and the same words. A slow call holds the box's one slot.
  std::thread slow([&] { fetch(port, bhttp::verb::get, "/api/spin"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto busy = fetch(port, bhttp::verb::get, "/api/boom", {{"accept", "application/json"}});
  slow.join();
  EXPECT_EQ(busy.status, 503);
  EXPECT_EQ(busy.headers["retry-after"], "1");
  EXPECT_NE(busy.body.find(R"("error":"rate limit reached")"), std::string::npos) << busy.body;
  expect_says_only_rate_limit_reached(busy, never);
  // The access log keeps which control and why.
  auto log = access_log();
  EXPECT_NE(log.find(R"("control":{"id":"secret_rule_name","outcome":"refused","reason":"rate")"), std::string::npos);
  EXPECT_NE(log.find(R"("id":"box:shop","outcome":"refused","reason":"concurrent")"), std::string::npos);
}

// The older host-wide limiter (limits.requests-per-minute) says the same, and only that.
TEST_F(ControlsTest, TheHostWideLimiterSaysOnlyRateLimitReached) {
  host.reset();
  start(R"(, "limits": {"requests-per-minute": 10})");
  Reply last;
  for (int i = 0; i < 12 && last.status != 429; ++i) last = fetch(port, bhttp::verb::get, "/api/items", {{"accept", "application/json"}});
  ASSERT_EQ(last.status, 429) << last.body;
  EXPECT_NE(last.body.find(R"("error":"rate limit reached")"), std::string::npos) << last.body;
  expect_says_only_rate_limit_reached(last, {"bucket", "\"all\"", "192.0.2", "127.0.0.1"});
  auto html = fetch(port, bhttp::verb::get, "/api/items", {{"accept", "text/html"}});
  EXPECT_EQ(html.status, 429);
  expect_says_only_rate_limit_reached(html, {"bucket", "127.0.0.1"});
}

// A box route's own limits (047 § 13): checked at upload, enforced per tenant per route,
// monitored as box:<box>/<route>; a box limit tighter than its API's control refuses first, and
// an operator's endpoint override beats the box.
TEST_F(ControlsTest, BoxRouteLimitsSitBetweenTheApiAndTheOperatorsOverride) {
  std::string err;
  std::string manifest = kManifest;
  manifest.replace(manifest.find(R"({"name":"items","public":true})"), 30,
                   R"({"name":"items","public":true,"limits":{"rate":{"per-minute":1,"burst":1}}})");
  // Out of range, and past the host's own limits, are refused at upload.
  auto refused = [&](const std::string& limits, const char* why) {
    std::string m = kManifest;
    m.replace(m.find(R"({"name":"items","public":true})"), 30, R"({"name":"items","public":true,"limits":)" + limits + "}");
    std::string e;
    EXPECT_FALSE(host->boxes().upload(m, kWat, &e)) << limits;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refused(R"({"rate":{"per-minute":0}})", "route items limits.rate.per-minute must be 0.1 to 1000000");
  refused(R"({"fuel":9000000000})", "route items limits.fuel must be a whole number, 1 to the host's limits.fuel max");
  refused(R"({"concurrent":{"max":0}})", "limits.concurrent.max must be a whole number, 1 to 100000");
  refused(R"({"rate":{"per-minute":5,"by":"tenant"}})", "limits.rate.by must be client, user, or all");
  // The checker (version_upload's dry run) says the same.
  std::string bad = kManifest;
  bad.replace(bad.find(R"({"name":"items","public":true})"), 30,
              R"({"name":"items","public":true,"limits":{"body-max-bytes":5}})");
  EXPECT_NE(error_of("version_upload", {{"manifest", bad}, {"wat", kWat}, {"dry_run", true}}).find("a get carries no body"),
            std::string::npos);
  auto dry = tool("version_upload", {{"manifest", manifest}, {"wat", kWat}, {"dry_run", true}});
  EXPECT_EQ(dry.at("ok"), true) << json::dump(dry);
  ASSERT_TRUE(host->boxes().upload(manifest, kWat, &err)) << err;

  // The API's looser rate doesn't save the route from its own.
  ASSERT_FALSE(tool("control_set", {{"domain", "example.com"},
                                    {"api", "box:shop"},
                                    {"set", bj::object{{"rate", bj::object{{"per-minute", 600}}}}},
                                    {"mode", "enforce"},
                                    {"why", "the shop's budget"}})
                   .as_object()
                   .contains("error"));
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 429);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/1", kSameSite).status, 200);  // other routes: the API's
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/1", kSameSite).status, 200);

  auto list = tool("control_list", {{"domain", "example.com"}});
  const bj::object* box = nullptr;
  for (const auto& c : list.at(0).at("controls").as_array()) {
    if (c.at("id") == "box:shop/items") box = &c.as_object();
  }
  ASSERT_TRUE(box) << json::dump(list);
  EXPECT_EQ(box->at("level"), "box");
  EXPECT_EQ(box->at("from"), "manifest");
  EXPECT_EQ(box->at("what"), "GET /api/items");
  EXPECT_EQ(box->at("limits").at("rate").at("per-minute"), 1);
  EXPECT_EQ(box->at("effective").at("enforced").at("rate").at("from"), "box:shop/items (the box's manifest)");
  auto report = tool("control_report", {{"domain", "example.com"}, {"id", "box:shop/items"}, {"window", "1h"}});
  ASSERT_FALSE(report.as_object().contains("error")) << json::dump(report);
  EXPECT_EQ(report.at("controls").at(0).at("totals").at("rate"), 1);
  EXPECT_NE(error_of("control_remove", {{"domain", "example.com"}, {"id", "box:shop/items"}}).find("from its manifest"),
            std::string::npos);

  // The operator's override that matches the route beats the box's bum limit, at once.
  ASSERT_FALSE(tool("control_set", {{"domain", "example.com"},
                                    {"endpoint", bj::object{{"id", "items_ok"}, {"match", "GET /api/items"}}},
                                    {"set", bj::object{{"rate", bj::object{{"per-minute", 600}}}}},
                                    {"mode", "enforce"},
                                    {"why", "the box's limit is bum"}})
                   .as_object()
                   .contains("error"));
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/api/items").status, 200);

  // version_get gathers the limits.
  auto live = host->boxes().active()->boxes.at("shop")->version;
  auto got = tool("version_get", {{"box", "shop"}, {"version", live}});
  EXPECT_EQ(got.at("limits").at(0).at("id"), "box:shop/items") << json::dump(got);
}

// A route's limits.fuel is the call's fuel: a box that spins traps out of fuel at once, rather
// than waiting out its timeout; an operator's fuel "off" gives the box its own fuel back.
TEST_F(ControlsTest, FuelLimitsTrapTheCall) {
  std::string err;
  std::string manifest = kManifest;
  manifest.replace(manifest.find(R"({"name":"spin","public":true})"), 29,
                   R"({"name":"spin","public":true,"limits":{"fuel":100000}})");
  ASSERT_TRUE(host->boxes().upload(manifest, kWat, &err)) << err;
  auto t0 = std::chrono::steady_clock::now();
  auto r = fetch(port, bhttp::verb::get, "/api/spin");
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_EQ(r.status, 500);
  EXPECT_LT(ms, 1500);
  EXPECT_NE(access_log().find(R"("outcome":"out_of_fuel")"), std::string::npos);
  // Lifted for the endpoint, with a short timeout so the test doesn't wait the box's 5 s: now a timeout.
  ASSERT_FALSE(tool("control_set", {{"domain", "example.com"},
                                    {"endpoint", bj::object{{"id", "spin_fuel"}, {"match", "GET /api/spin"}}},
                                    {"set", bj::object{{"fuel", "off"}, {"timeout-ms", 200}}},
                                    {"mode", "enforce"},
                                    {"why", "the box's fuel is too low"}})
                   .as_object()
                   .contains("error"));
  fetch(port, bhttp::verb::get, "/api/spin");
  EXPECT_NE(access_log().find(R"("outcome":"timeout")"), std::string::npos);
}

// A reload that lowers the host's own limits reaches box route limits at once: a route limit
// past the new limit is no longer applied, and control_list says so (047 § 13).
TEST_F(ControlsTest, BoxRouteLimitsAreCheckedAgainAfterAReload) {
  std::string err;
  std::string manifest = kManifest;
  manifest.replace(manifest.find(R"({"name":"cart","public":true})"), 29,
                   R"({"name":"cart","public":true,"limits":{"body-max-bytes":100}})");
  ASSERT_TRUE(host->boxes().upload(manifest, kWat, &err)) << err;
  auto entry = [&]() -> bj::object {
    auto list = tool("control_list", {{"domain", "example.com"}});
    for (const auto& c : list.at(0).at("controls").as_array()) {
      if (c.at("id") == "box:shop/cart") return c.as_object();
    }
    return {};
  };
  ASSERT_FALSE(entry().empty());
  EXPECT_FALSE(entry().contains("note")) << json::dump(entry());
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/api/cart/1", kSameSite, std::string(101, 'x')).status, 413);
  // host.json now says 50 bytes: the box's 100 is past it.
  auto lowered = config::parse_config(config_text(R"(, "limits": {"body-max-bytes": 50})"), root, &err);
  ASSERT_TRUE(lowered) << err;
  host->config().replace(config::local_mode(lowered));
  auto after = entry();
  ASSERT_FALSE(after.empty());
  ASSERT_TRUE(after.contains("note")) << json::dump(after);
  EXPECT_NE(std::string(after.at("note").as_string()).find("not applied"), std::string::npos) << json::dump(after);
}
