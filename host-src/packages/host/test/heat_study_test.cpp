// Heat studies in the host (design/machine/048.HeatStudy.md): requests keyed by endpoint from the
// real request path, the tools, approvals and the panel, the operator scope, the audit log, the
// end of a study (time up, a host stopping), and nothing private in the report.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>

#include "goober/operations/heat.h"
#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"
#include "goober/json/json.h"
#include "goober/logs/logs.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

struct Reply {
  int status = 0;
  std::string body;
};

Reply fetch(uint16_t port, const std::string& host, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, host);
  for (const auto& [k, v] : headers) req.set(k, v);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  return Reply{int(res.result_int()), res.body()};
}

const char* kShopManifest = R"({"box":"shop","domains":["*"],
  "routes":{"http":{
    "/items/{id : int64}":{"get":{"name":"item","public":true}},
    "/members":{"get":{"name":"members","requirements":[["member"]]}}}}})";

const char* kShop = R"wat((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22item\22:true}")
  (func (export "goober_route_item") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 13)))
    (i32.const 0))
  (func (export "goober_route_members") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 13)))
    (i32.const 0))
))wat";

const char* kOpToken = "op-token-for-heat-tests-0123456789";

}  // namespace

class HeatStudyHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "heat_study_host";
    fs::remove_all(root);
    start_host();
    std::string err;
    grok = *host::create_operator_token(root / "storage", "grok", 0, &err);
    ASSERT_TRUE(host->boxes().upload(kShopManifest, kShop, &err)) << err;
  }
  void TearDown() override { host.reset(); }
  void start_host() {
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"acme.example.com": {"tenant": "acme"}, "other.example.com": {"tenant": "other"}},
        "operations": {"token": "op-token-for-heat-tests-0123456789"},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }

  // The tool's answer as {isError, value, text}.
  bj::object call(const std::string& bearer, const std::string& tool, const bj::object& args = {}) {
    auto r = fetch(port, "acme.example.com", bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer " + bearer}, {"content-type", "application/json"}},
                   json::dump(bj::object{{"jsonrpc", "2.0"},
                                         {"id", 1},
                                         {"method", "tools/call"},
                                         {"params", bj::object{{"name", tool}, {"arguments", args}}}}));
    std::string err;
    auto v = json::parse(r.body, &err);
    if (!v || !v->as_object().contains("result")) return bj::object{{"isError", true}, {"text", r.body}};
    auto res = v->as_object().at("result").as_object();
    bj::object out{{"isError", res.at("isError")}, {"text", res.at("content").at(0).at("text")}};
    if (res.contains("structuredContent")) {
      out["value"] = res.at("structuredContent");
    } else if (auto parsed = json::parse(std::string(res.at("content").at(0).at("text").as_string()), &err)) {
      out["value"] = *parsed;
    }
    return out;
  }
  bj::object op(const std::string& tool, const bj::object& args = {}) { return call(kOpToken, tool, args); }
  std::string audit_text() {
    std::string all;
    for (const auto& f : logs::files_of(root / "storage/logs", "audit")) {
      std::ifstream in(f);
      std::string l;
      while (std::getline(in, l)) all += l + "\n";
    }
    return all;
  }
  static const bj::object* endpoint(const bj::value& rep, const std::string& name) {
    for (const auto& e : rep.at("endpoints").as_array()) {
      if (e.at("endpoint").as_string() == name) return &e.as_object();
    }
    return nullptr;
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string grok;
};

TEST_F(HeatStudyHostTest, RequestsAreKeyedByEndpointAndGetSuggestions) {
  ASSERT_FALSE(op("page_put", {{"tenant", "acme"}, {"path", "/about"}, {"format", "markdown"}, {"body", "about us"}})
                   .at("isError")
                   .as_bool());
  auto started = op("heat_study_start", {{"domain", "acme.example.com"}, {"minutes", 30}, {"why", "busy hour"}});
  ASSERT_FALSE(started.at("isError").as_bool()) << started.at("text");
  int64_t id = started.at("value").at("id").as_int64();
  EXPECT_EQ(started.at("value").at("tenant").as_string(), "acme");

  for (int i = 0; i < 25; ++i) {
    auto r = fetch(port, "acme.example.com", bhttp::verb::get, "/items/" + std::to_string(i),
                   {{"user-agent", "Mozilla/5.0 (X11; Linux x86_64)"}});
    ASSERT_EQ(r.status, 200) << r.body;
  }
  for (int i = 0; i < 3; ++i) fetch(port, "acme.example.com", bhttp::verb::get, "/members", {{"user-agent", "curl/8.5.0"}});
  for (int i = 0; i < 2; ++i) fetch(port, "acme.example.com", bhttp::verb::get, "/about");
  fetch(port, "acme.example.com", bhttp::verb::get, "/no/such/page");
  fetch(port, "other.example.com", bhttp::verb::get, "/items/1");  // not studied
  // The host's own paths aren't studied at all: /goober/* and CORS preflights.
  fetch(port, "acme.example.com", bhttp::verb::get, "/goober/ready");
  fetch(port, "acme.example.com", bhttp::verb::get, "/goober/health");
  fetch(port, "acme.example.com", bhttp::verb::options, "/items/1",
        {{"origin", "https://elsewhere.example.com"}, {"access-control-request-method", "GET"}});

  auto got = op("heat_study_get", {{"id", id}});
  ASSERT_FALSE(got.at("isError").as_bool()) << got.at("text");
  const auto& rep = got.at("value");
  EXPECT_EQ(rep.at("state").as_string(), "running");
  EXPECT_GT(rep.at("remaining_ms").as_int64(), 0);
  // The operator's own tool calls on the domain aren't its traffic, nor the host's own paths.
  EXPECT_EQ(rep.at("totals").at("requests").as_int64(), 31) << json::dump(rep);
  EXPECT_EQ(json::dump(rep).find("/goober"), std::string::npos);
  EXPECT_EQ(json::dump(rep).find("preflight"), std::string::npos);

  const auto* items = endpoint(rep, "GET /items/{id}");
  ASSERT_TRUE(items) << json::dump(rep);
  EXPECT_EQ(items->at("api").as_string(), "box:shop");
  EXPECT_EQ(items->at("requests").as_int64(), 25);
  EXPECT_EQ(items->at("status").at("2xx").as_int64(), 25);
  EXPECT_EQ(items->at("clients").at("distinct").as_int64(), 1);
  EXPECT_GT(items->at("bytes").at("out").as_int64(), 0);
  EXPECT_GT(items->at("heat_us").as_int64(), 0);
  const auto& suggest = items->at("suggest").as_object();
  ASSERT_TRUE(suggest.contains("control_set")) << json::dump(suggest);
  EXPECT_EQ(suggest.at("control_set").at("mode").as_string(), "observe");
  EXPECT_EQ(suggest.at("control_set").at("endpoint").at("match").as_string(), "GET /items/{id}");
  EXPECT_EQ(suggest.at("control_set").at("domain").as_string(), "acme.example.com");
  EXPECT_EQ(suggest.at("control_set").at("set").at("rate").at("by").as_string(), "client");
  EXPECT_TRUE(suggest.at("control_set").at("set").as_object().contains("timeout-ms"));
  // A suggestion is control_set's own arguments: sent as it is, it's taken, for a route and for
  // one MCP tool alike.
  {
    auto set = op("control_set", suggest.at("control_set").as_object());
    EXPECT_FALSE(set.at("isError").as_bool()) << set.at("text");
    operations::SuggestInput in;
    in.domain = "acme.example.com";
    in.api = "mcp";
    in.endpoint = "search";
    in.study_id = 1;
    in.requests = 100;
    in.clients = 3;
    in.p99_client_minute = 30;
    in.peak_minute = 40;
    in.peak_concurrent = 2;
    in.peak_client_concurrent = 1;
    in.max_body_bytes = 500;
    auto tool = operations::suggest_controls(in);
    ASSERT_TRUE(tool.contains("control_set")) << json::dump(tool);
    auto set_tool = op("control_set", tool.at("control_set").as_object());
    EXPECT_FALSE(set_tool.at("isError").as_bool()) << set_tool.at("text");
    for (const char* id : {"get_items_id", "tool_search"}) {
      auto rm = op("control_remove", {{"domain", "acme.example.com"}, {"id", id}, {"why", "test"}});
      EXPECT_FALSE(rm.at("isError").as_bool()) << id << ": " << rm.at("text");
    }
  }

  // The host refusing before the box ran is still the route's.
  const auto* members = endpoint(rep, "GET /members");
  ASSERT_TRUE(members) << json::dump(rep);
  EXPECT_EQ(members->at("status").at("4xx").as_int64(), 3);
  const auto* about = endpoint(rep, "GET /about");
  ASSERT_TRUE(about);
  EXPECT_EQ(about->at("api").as_string(), "content");
  EXPECT_TRUE(about->at("suggest").as_object().contains("none"));  // two requests
  ASSERT_TRUE(endpoint(rep, "(not found)"));
  EXPECT_EQ(rep.at("endpoints").as_array().size(), 4u) << json::dump(rep);

  // Nothing private: no address, no user id, only keyed hashes of clients.
  std::string text = json::dump(rep);
  EXPECT_EQ(text.find("127.0.0.1"), std::string::npos);
  EXPECT_EQ(text.find("user_id"), std::string::npos);
  EXPECT_EQ(rep.at("clients").at("top").at(0).at("client_h").as_string().size(), 16u);

  // Listed, then stopped early with what was gathered kept.
  auto list = op("heat_study_list", {{"domain", "acme.example.com"}});
  ASSERT_EQ(list.at("value").as_array().size(), 1u);
  auto stopped = op("heat_study_stop", {{"id", id}});
  ASSERT_FALSE(stopped.at("isError").as_bool()) << stopped.at("text");
  EXPECT_EQ(stopped.at("value").at("state").as_string(), "stopped");
  fetch(port, "acme.example.com", bhttp::verb::get, "/items/1");
  auto after = op("heat_study_get", {{"id", id}});
  EXPECT_EQ(after.at("value").at("totals").at("requests").as_int64(), 31);
  EXPECT_TRUE(fs::exists(root / "storage/operations/studies" / (std::to_string(id) + ".json")));
  // Audited both ways.
  std::string audit = audit_text();
  EXPECT_NE(audit.find("study.start"), std::string::npos);
  EXPECT_NE(audit.find("study.stop"), std::string::npos);
  // Its bytes count against limits.studies-bytes.
  auto health = op("host_health");
  EXPECT_TRUE(health.at("value").at("ready").at("checks").at("limits").as_object().contains("studies"))
      << json::dump(health.at("value").at("ready").at("checks").at("limits"));
}

// The hook's whole cost per request with a study running (048 § 3.1): keying a box route by its
// pattern, the client's keyed hash, its class, and the accumulators. Printed; bounded loosely.
TEST_F(HeatStudyHostTest, HookCost) {
  ASSERT_FALSE(op("heat_study_start", {{"domain", "acme.example.com"}, {"minutes", 60}, {"why", "cost"}}).at("isError").as_bool());
  std::vector<server::http::Request> reqs;
  for (int i = 0; i < 2000; ++i) {
    server::http::Request r;
    r.method = i % 5 ? "get" : "head";
    r.path = i % 3 ? "/items/" + std::to_string(i) : "/members";
    r.target = r.path;
    r.remote = "198.51.100." + std::to_string(i % 250);
    r.headers = {{"host", "acme.example.com"},
                 {"user-agent", "Mozilla/5.0 (X11; Linux x86_64; rv:130.0) Gecko/20100101 Firefox/130.0"},
                 {"accept", "text/html"}};
    reqs.push_back(std::move(r));
  }
  bj::object line{{"status", 200}, {"us", 850}, {"route", "item"}, {"box", "shop"}, {"fuel", 1200}};
  constexpr int kN = 200000;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) {
    if (host->heat_studies()->watching("acme.example.com")) host->heat_observe(reqs[size_t(i) % reqs.size()], line, "acme.example.com", 13);
  }
  double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / kN;
  std::printf("heat study hook: %.0f ns a request with a study running\n", ns);
  auto rep = op("heat_study_get", {{"id", 1}});
  EXPECT_EQ(rep.at("value").at("totals").at("requests").as_int64(), kN);
  EXPECT_LT(ns, 200000);
}

TEST_F(HeatStudyHostTest, StartIsCheckedAndOnePerDomain) {
  EXPECT_TRUE(op("heat_study_start", {{"domain", "nope.example.com"}, {"why", "x"}}).at("isError").as_bool());
  EXPECT_TRUE(op("heat_study_start", {{"domain", "acme.example.com"}}).at("isError").as_bool());  // no why
  auto short_one = op("heat_study_start", {{"domain", "acme.example.com"}, {"minutes", 2}, {"why", "x"}});
  EXPECT_TRUE(short_one.at("isError").as_bool());
  ASSERT_FALSE(op("heat_study_start", {{"domain", "acme.example.com"}, {"why", "x"}}).at("isError").as_bool());
  auto twice = op("heat_study_start", {{"domain", "acme.example.com"}, {"why", "x"}});
  EXPECT_TRUE(twice.at("isError").as_bool());
  EXPECT_NE(std::string(twice.at("text").as_string()).find("one per domain"), std::string::npos);
  EXPECT_EQ(op("heat_study_list").at("value").at(0).at("minutes").as_int64(), 60);  // the default
}

TEST_F(HeatStudyHostTest, NeverWaitsForApprovalAndThePanelOnlyReads) {
  // A read call for approvals (it changes nothing served), but not a read tool, so observers and
  // the look-only panel can't start or stop one.
  EXPECT_TRUE(host::Host::is_read_call("heat_study_start", {}));
  EXPECT_TRUE(host::Host::is_read_call("heat_study_stop", {}));
  EXPECT_FALSE(host::Host::is_read_tool("heat_study_start"));
  EXPECT_FALSE(host::Host::is_read_tool("heat_study_stop"));
  EXPECT_TRUE(host::Host::is_read_tool("heat_study_get"));
  EXPECT_TRUE(host::Host::is_read_tool("heat_study_list"));
  for (const char* t : {"heat_study_start", "heat_study_stop", "heat_study_get", "heat_study_list"}) {
    EXPECT_TRUE(host::Host::scope_rule(t).has_value()) << t;
  }
}

TEST_F(HeatStudyHostTest, AFocusedOperatorStudiesOnlyItsTenants) {
  host::Host::OperatorScope acme;
  acme.all = false;
  acme.tenants = {"acme"};
  std::string why;
  ASSERT_TRUE(host->set_operator_scope("token:grok", acme, "test", &why)) << why;
  auto refused = call(grok, "heat_study_start", {{"domain", "other.example.com"}, {"why", "x"}});
  ASSERT_TRUE(refused.at("isError").as_bool());
  EXPECT_NE(std::string(refused.at("text").as_string()).find("isn't one of theirs"), std::string::npos) << refused.at("text");
  auto mine = call(grok, "heat_study_start", {{"domain", "acme.example.com"}, {"why", "x"}});
  ASSERT_FALSE(mine.at("isError").as_bool()) << mine.at("text");
  // Another tenant's study, started by an operator with every tenant.
  auto theirs = op("heat_study_start", {{"domain", "other.example.com"}, {"why", "x"}});
  ASSERT_FALSE(theirs.at("isError").as_bool()) << theirs.at("text");
  int64_t other_id = theirs.at("value").at("id").as_int64();
  auto peek = call(grok, "heat_study_get", {{"id", other_id}});
  ASSERT_TRUE(peek.at("isError").as_bool());
  // Outside the scope, and whose it is isn't said (no tenant named).
  EXPECT_NE(std::string(peek.at("text").as_string()).find("outside your scope"), std::string::npos) << peek.at("text");
  EXPECT_EQ(std::string(peek.at("text").as_string()).find("other"), std::string::npos) << peek.at("text");
  EXPECT_TRUE(call(grok, "heat_study_stop", {{"id", other_id}}).at("isError").as_bool());
  // The same id in another shape (a whole double, a string of digits) is the same study, and is
  // refused the same; a shape that isn't a whole number is refused, never read another way.
  for (const bj::value& id : {bj::value(double(other_id)), bj::value(std::to_string(other_id)),
                              bj::value(double(other_id) + 0.5), bj::value(true), bj::value(bj::array{other_id})}) {
    for (const char* tool : {"heat_study_get", "heat_study_stop"}) {
      auto r = call(grok, tool, {{"id", id}});
      EXPECT_TRUE(r.at("isError").as_bool()) << tool << " " << json::dump(id) << " " << r.at("text");
    }
  }
  // The other tenant's study is still running: no shape of its id stopped it.
  auto still = op("heat_study_get", {{"id", other_id}});
  EXPECT_EQ(still.at("value").at("state").as_string(), "running") << still.at("text");
  auto list = call(grok, "heat_study_list");
  ASSERT_EQ(list.at("value").as_array().size(), 1u) << list.at("text");
  EXPECT_EQ(list.at("value").at(0).at("tenant").as_string(), "acme");
}

TEST_F(HeatStudyHostTest, EndsWhenTimeIsUpAndWhenTheHostStops) {
  auto a = op("heat_study_start", {{"domain", "acme.example.com"}, {"minutes", 5}, {"why", "x"}});
  ASSERT_FALSE(a.at("isError").as_bool());
  int64_t first = a.at("value").at("id").as_int64();
  fetch(port, "acme.example.com", bhttp::verb::get, "/items/1");
  host->heat_tick(base::now_unix_ms() + 6 * 60000);
  auto done = op("heat_study_get", {{"id", first}});
  EXPECT_EQ(done.at("value").at("state").as_string(), "done");
  EXPECT_EQ(done.at("value").at("ended_by").as_string(), "time");
  EXPECT_EQ(done.at("value").at("totals").at("requests").as_int64(), 1);

  auto b = op("heat_study_start", {{"domain", "acme.example.com"}, {"why", "x"}});
  ASSERT_FALSE(b.at("isError").as_bool());
  int64_t second = b.at("value").at("id").as_int64();
  fetch(port, "acme.example.com", bhttp::verb::get, "/items/2");
  fetch(port, "acme.example.com", bhttp::verb::get, "/items/3");
  // A graceful stop ends it with what it gathered; the next start reads the report back.
  host.reset();
  start_host();
  auto kept = op("heat_study_get", {{"id", second}});
  ASSERT_FALSE(kept.at("isError").as_bool()) << kept.at("text");
  EXPECT_EQ(kept.at("value").at("state").as_string(), "stopped");
  EXPECT_EQ(kept.at("value").at("ended_by").as_string(), "host_stopping");
  EXPECT_EQ(kept.at("value").at("totals").at("requests").as_int64(), 2);
  EXPECT_EQ(op("heat_study_list").at("value").as_array().size(), 2u);
}
