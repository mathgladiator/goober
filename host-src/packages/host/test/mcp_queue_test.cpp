// MCP's strict rate and fixed queue through a real host (design/machine/047.Controls.md § 14):
// the rate per agent and per call (batches too), the refusal that says only "rate limit
// reached", a flooding agent that can't starve a polite one or take every worker, a client gone
// while its call waits, the report, control_report's section, and /goober/ready's ratios.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <thread>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kToken = "mcp-queue-token-0123456789abcdef";

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
  // A batch of a hundred whats_new answers is past beast's 8 MB default.
  bhttp::response_parser<bhttp::string_body> parser;
  parser.body_limit(std::uint64_t(256) << 20);
  bhttp::read(stream, buf, parser);
  auto res = parser.release();
  Reply r{int(res.result_int()), res.body(), {}};
  for (const auto& f : res) r.headers[base::to_lower(std::string(f.name_string()))] = std::string(f.value());
  return r;
}

// fast answers at once; slow spins until the box's timeout (400 ms) ends it.
const char* kWat = R"wat((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22ok\22:true}")
  (func (export "goober_route_fast") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 11))) (i32.const 0))
  (func (export "goober_route_page") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 11))) (i32.const 0))
  (func (export "goober_route_slow") (result i32) (loop $l (br $l)) (i32.const 0))
))wat";

const char* kManifest = R"({"box":"tools","domains":["example.com"],
    "behavior":{"timeout-ms":400,"fuel":2000000000},
    "routes":{
      "http":{"/page":{"get":{"name":"page","public":true}}},
      "mcp":{
        "fast":{"tool":{"description":"Answers at once.","requirements":[["owner"]]}},
        "slow":{"tool":{"description":"Takes 400 ms, then fails.","requirements":[["owner"]]}}}}})";

class McpQueueTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "mcp_queue";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(std::string(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"example.com": {"tenant": "example"}}, "operations": {"token": ")") +
                                        kToken + R"("}, "threads": {"network": 2, "boxes": 4}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    config = config::local_mode(cfg);
    host = std::make_unique<host::Host>(config);
    port = host->start();
    ASSERT_TRUE(host->boxes().upload(kManifest, kWat, &err)) << err;
    auto& id = host->identity();
    int64_t now = base::now_unix_ms();
    ASSERT_TRUE(id.grant("example", "exec@example.com", "owner", "test", now, &err)) << err;
    user = std::stoll(std::string(id.describe("example", now).at("users").at(0).at("user_id").as_string()));
    for (const char* name : {"Grok", "Polite"}) {
      auto client = id.register_client("example", name, {}, now, &err);
      ASSERT_TRUE(client) << err;
      clients.push_back(client->client_id);
      tokens.push_back(device_token(client->client_id));
      ASSERT_FALSE(tokens.back().empty());
    }
  }
  // A fresh access token for the client and the person, by the device flow.
  std::string device_token(const std::string& client_id) {
    auto& id = host->identity();
    std::string err;
    identity::OAuthError e;
    auto d = id.start_device("example", client_id, base::now_unix_ms(), &e);
    if (!d) return "";
    if (!id.decide_device("example", d->user_code, user, true, base::now_unix_ms(), &err)) return "";
    auto t = id.poll_device("example", client_id, d->device_code, base::now_unix_ms(), 86400000, &e);
    return t ? t->access_token : "";
  }
  void TearDown() override { host.reset(); }

  Reply call(const std::string& token, const std::string& tool, int id = 1) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                   {"params", bj::object{{"name", tool}, {"arguments", bj::object{}}}}};
    return fetch(port, bhttp::verb::post, "/mcp", {{"authorization", "Bearer " + token}, {"content-type", "application/json"}},
                 json::dump(req));
  }
  Reply op(const std::string& body) {
    return fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                 {{"authorization", std::string("Bearer ") + kToken}, {"content-type", "application/json"}}, body);
  }
  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = op(json::dump(req));
    std::string err;
    auto v = json::parse(r.body, &err);
    if (!v || !v->as_object().contains("result")) return bj::object{{"error", r.body}};
    const auto& res = v->at("result").as_object();
    std::string text(res.at("content").at(0).at("text").as_string());
    if (res.at("isError").as_bool()) return bj::object{{"error", text}};
    if (res.contains("structuredContent")) return res.at("structuredContent");
    return *json::parse(text, &err);
  }
  template <class F>
  bool eventually(F f, int ms = 5000) {
    for (auto end = Clock::now() + std::chrono::milliseconds(ms); Clock::now() < end;) {
      if (f()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
  }
  std::string log(const char* name) { return base::read_file(root / "storage" / "logs" / name).value_or(""); }

  fs::path root;
  std::shared_ptr<const config::HostConfig> config;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  int64_t user = 0;
  std::vector<std::string> tokens, clients;
};

size_t count(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

}  // namespace

// Each agent has its own strict rate; a refusal says only "rate limit reached".
TEST_F(McpQueueTest, AStrictRatePerAgentAndARefusalThatSaysOnlyThat) {
  int first_refused = -1;
  Reply refused;
  for (int i = 0; i < 40 && first_refused < 0; ++i) {
    auto r = call(tokens[0], "fast", 100 + i);
    if (r.status != 200) first_refused = i, refused = r;
  }
  // The first call counts as the client address's (the token isn't known good yet), then the
  // agent's own burst of 30.
  EXPECT_EQ(first_refused, 31);
  EXPECT_EQ(refused.status, 429);
  EXPECT_EQ(refused.headers["retry-after"], "1");
  EXPECT_EQ(refused.body, R"({"jsonrpc":"2.0","id":131,"error":{"code":-32000,"message":"rate limit reached"}})");
  // Another agent isn't slowed by it.
  EXPECT_EQ(call(tokens[1], "fast").status, 200);
  // The access log says why; the client was told nothing of it.
  EXPECT_NE(log("access.log").find(R"("mcp_queue":"rate")"), std::string::npos);
  // A batch on /mcp is counted call by call before the handler refuses batches at all.
  std::string batch = "[";
  for (int i = 0; i < 31; ++i) batch += std::string(i ? "," : "") + R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
  batch += "]";
  auto big = fetch(port, bhttp::verb::post, "/mcp", {{"authorization", "Bearer " + tokens[1]}, {"content-type", "application/json"}}, batch);
  EXPECT_EQ(big.status, 400);
  EXPECT_NE(big.body.find("a batch holds at most 30 calls"), std::string::npos) << big.body;
}

// The operator MCP takes batches: each call in one counts against the operator's rate.
TEST_F(McpQueueTest, TheOperatorMcpCountsEveryCallOfABatch) {
  auto batch = [](int n) {
    std::string b = "[";
    for (int i = 0; i < n; ++i) {
      b += std::string(i ? "," : "") + R"({"jsonrpc":"2.0","id":)" + std::to_string(i) +
           R"(,"method":"tools/call","params":{"name":"whats_new","arguments":{}}})";
    }
    return b + "]";
  };
  auto too_big = op(batch(121));
  EXPECT_EQ(too_big.status, 400);
  EXPECT_NE(too_big.body.find("a batch holds at most 120 calls"), std::string::npos);
  // The first call is the address's; then 100 of the operator's 120.
  EXPECT_EQ(op(R"({"jsonrpc":"2.0","id":1,"method":"ping"})").status, 200);
  EXPECT_EQ(op(batch(100)).status, 200);
  auto refused = op(batch(30));
  EXPECT_EQ(refused.status, 429);
  EXPECT_EQ(refused.body, R"({"jsonrpc":"2.0","id":null,"error":{"code":-32000,"message":"rate limit reached"}})");
  EXPECT_EQ(op(batch(15)).status, 200);  // what fits still runs
  // The operator queue's report.
  auto report = tool("mcp_queue_report", {{"window", "1h"}});
  ASSERT_FALSE(report.as_object().contains("error")) << json::dump(report);
  EXPECT_EQ(report.at("queue"), "operator");
  EXPECT_EQ(report.at("limits").at("burst").as_double(), 120);
  EXPECT_GE(report.at("totals").at("refused_rate").as_int64(), 30);
  EXPECT_TRUE(report.at("by_class").as_object().contains("operator"));
  EXPECT_NE(tool("mcp_queue_report", {{"domain", "nowhere.example"}}).at("error").as_string().find("no domain"), std::string::npos);
}

// A misbehaving agent floods; a polite one is still served in turn, other requests are served
// at once (MCP never takes every worker), and the monitoring shows what happened.
TEST_F(McpQueueTest, AFloodingAgentNeitherStarvesAPoliteOneNorTakesTheWorkers) {
  // Both tokens proved good first, so each counts as its own agent from the first call of the flood.
  ASSERT_EQ(call(tokens[0], "fast").status, 200);
  ASSERT_EQ(call(tokens[1], "fast").status, 200);
  std::mutex mu;
  std::map<int, int> statuses;
  std::vector<std::thread> flood;
  for (int i = 0; i < 40; ++i) {
    flood.emplace_back([&, i] {
      auto r = call(tokens[0], "slow", i);
      std::lock_guard lock(mu);
      ++statuses[r.status];
    });
  }
  // Wait until the flood fills its queue: two running, the rest waiting, no worker held by them.
  ASSERT_TRUE(eventually([&] { return host->mcp_queue_stats().waiting >= 20; })) << host->mcp_queue_stats().waiting;
  EXPECT_LE(host->mcp_queue_stats().running, 2);
  // Everything else is answered at once: workers are left for it.
  auto t0 = Clock::now();
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/page").status, 200);
  EXPECT_LT(Clock::now() - t0, std::chrono::milliseconds(300));
  // The polite agent waits for one slot, not behind the whole flood (14 rounds of 400 ms).
  t0 = Clock::now();
  auto r = call(tokens[1], "fast");
  auto took = Clock::now() - t0;
  EXPECT_EQ(r.status, 200) << r.body;
  EXPECT_LT(took, std::chrono::milliseconds(2000));
  EXPECT_GT(host->mcp_queue_stats().waiting, 0);  // the flood is still waiting its turns
  for (auto& t : flood) t.join();
  // The rate admitted the burst of 30; the queue held every one of those.
  EXPECT_EQ(statuses[503], 0);
  EXPECT_GE(statuses[429], 9);
  EXPECT_EQ(statuses[200] + statuses[429], 40);
  EXPECT_EQ(host->mcp_queue_stats().running, 0);
  EXPECT_EQ(host->mcp_queue_stats().waiting, 0);

  // The report: queued, ran, refused at the rate, by class.
  auto report = tool("mcp_queue_report", {{"domain", "example.com"}, {"window", "1h"}});
  ASSERT_FALSE(report.as_object().contains("error")) << json::dump(report);
  const auto& t = report.at("totals");
  EXPECT_GE(t.at("queued").as_int64(), 20);
  EXPECT_GE(t.at("refused_rate").as_int64(), 8);
  EXPECT_GE(t.at("peak_waiting").as_int64(), 20);
  EXPECT_GT(t.at("wait_p99_ms").as_double(), 400);
  EXPECT_TRUE(report.at("by_class").as_object().contains("agent"));
  EXPECT_NE(json::dump(report.at("verdicts")).find("at the strict rate"), std::string::npos);
  // control_report carries the domain's queue too.
  auto controls = tool("control_report", {{"domain", "example.com"}, {"window", "1h"}});
  EXPECT_GE(controls.at("mcp_queue").at("totals").at("queued").as_int64(), 20) << json::dump(controls);
  // A waited call's access line says how long it waited.
  EXPECT_NE(log("access.log").find(R"("mcp_queue":{"waited_ms":)"), std::string::npos);
  // /goober/ready: ratios and a latency, never counts or names.
  auto ready = fetch(port, bhttp::verb::get, "/goober/ready");
  std::string err;
  auto body = json::parse(ready.body, &err);
  ASSERT_TRUE(body) << err;
  const auto& m = body->at("health").at("mcp").as_object();
  EXPECT_GT(m.at("waited_ratio").as_double(), 0.3);
  EXPECT_GT(m.at("refused_ratio").as_double(), 0.1);
  EXPECT_FALSE(m.contains("offered"));
  EXPECT_EQ(ready.body.find("Grok"), std::string::npos);
  auto prom = fetch(port, bhttp::verb::get, "/goober/ready?format=prometheus");
  EXPECT_NE(prom.body.find("goober_mcp_waited_ratio "), std::string::npos);
  EXPECT_NE(prom.body.find("goober_mcp_refused_ratio "), std::string::npos);
  // The rows are kept in traffic.db (7 days, within limits.traffic-bytes): a restart still has them.
  host.reset();
  host = std::make_unique<host::Host>(config);
  port = host->start();
  auto kept = host->mcp_queue_report("example.com", "1h");
  EXPECT_GE(kept.at("totals").at("queued").as_int64(), 20) << json::dump(kept);
}

// A call whose client hangs up while it waits is taken out of the queue and never run.
TEST_F(McpQueueTest, ACallWhoseClientLeftWhileWaitingNeverRuns) {
  call(tokens[0], "fast");  // the token is known good from here
  std::vector<std::thread> running;
  for (int i = 0; i < 2; ++i) running.emplace_back([&] { call(tokens[0], "slow"); });
  ASSERT_TRUE(eventually([&] { return host->mcp_queue_stats().running == 2; }));
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(bhttp::verb::post, "/mcp", 11);
  req.set(bhttp::field::host, "example.com");
  req.set("authorization", "Bearer " + tokens[0]);
  req.set("content-type", "application/json");
  req.body() = R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"fast","arguments":{}}})";
  req.prepare_payload();
  bhttp::write(stream, req);
  ASSERT_TRUE(eventually([&] { return host->mcp_queue_stats().waiting == 1; }));
  stream.socket().close();
  // The sweep notices within a quarter second, well before a slot frees.
  EXPECT_TRUE(eventually([&] { return host->mcp_queue_stats().waiting == 0; }, 350));
  for (auto& t : running) t.join();
  EXPECT_NE(log("access.log").find(R"("mcp_queue":"gone")"), std::string::npos);
  // The agents log has each call that ran: the first fast one and the two slow ones, not the third.
  auto agents = log("agents.log");
  EXPECT_EQ(count(agents, R"("tool":"fast")"), 1u) << agents;
  EXPECT_EQ(count(agents, R"("tool":"slow")"), 2u);
}

// A token revoked after it was known good stops counting as its agent's at its first refusal:
// its calls go back to the address's bucket, so they never drain the agent's (the person's next
// token still has the agent's whole burst).
TEST_F(McpQueueTest, ARevokedTokenStopsDrainingItsAgentsBucket) {
  ASSERT_EQ(call(tokens[0], "fast").status, 200);  // known good from here
  host->identity().revoke_token("example", clients[0], tokens[0], base::now_unix_ms());
  for (int i = 0; i < 12; ++i) EXPECT_EQ(call(tokens[0], "fast").status, 401) << i;
  std::string fresh = device_token(clients[0]);
  ASSERT_FALSE(fresh.empty());
  ASSERT_EQ(call(fresh, "fast").status, 200);  // the address's, while the new token is proved
  // The same agent (the client and the person): its burst of 30 is whole but for the first call.
  for (int i = 0; i < 26; ++i) EXPECT_EQ(call(fresh, "fast").status, 200) << i;
}
