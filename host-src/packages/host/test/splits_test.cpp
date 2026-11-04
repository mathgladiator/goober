// A/B splits in the host (design/machine/052.Splits.md): requests assigned by a stable hash, both
// arms counted, the ladder's steps and promotion, guardrails rolling back on their own, the tools,
// and compatibility.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

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

const char* kManifest = R"({"box":"ab","domains":["*"],"routes":{"http":{"/ab":{"get":{"name":"ab","public":true}}}}})";

// Answers `status` with the body `text`.
std::string module_saying(int status, const std::string& text) {
  return R"((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "text/plain")
  (data (i32.const 32) ")" + text + R"(")
  (func (export "goober_route_ab") (result i32)
    (drop (call $respond (i32.const )" + std::to_string(status) + R"() (i32.const 0) (i32.const 10) (i32.const 32) (i32.const )" +
         std::to_string(text.size()) + R"()))
    (i32.const 0))))";
}

const char* kToken = "op-token-for-split-tests-0123456789";

}  // namespace

class SplitsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "splits";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0, "behind-proxy": true}, "storage": "storage",
        "domains": {"localhost": {"tenant": "acme"}}, "operations": {"token": "op-token-for-split-tests-0123456789"},
        "threads": {"network": 1, "boxes": 2}})", root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    auto v1 = host->boxes().upload(kManifest, module_saying(200, "one"), &err);
    ASSERT_TRUE(v1) << err;
    live = v1->version;
  }
  void TearDown() override { host.reset(); }
  std::string store(int status, const std::string& text) {
    std::string err;
    auto v = host->boxes().store(kManifest, module_saying(status, text), "", &err);
    EXPECT_TRUE(v) << err;
    return v ? v->version : "";
  }
  bj::object op(const std::string& tool, const bj::object& args) {
    auto r = fetch(port, "localhost", bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", std::string("Bearer ") + kToken}, {"content-type", "application/json"}},
                   json::dump(bj::object{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                         {"params", bj::object{{"name", tool}, {"arguments", args}}}}));
    std::string err;
    auto v = json::parse(r.body, &err);
    if (!v || !v->as_object().contains("result")) return bj::object{{"isError", true}, {"text", r.body}};
    auto res = v->as_object().at("result").as_object();
    bj::object out{{"isError", res.at("isError")}, {"text", res.at("content").at(0).at("text")}};
    if (auto parsed = json::parse(std::string(res.at("content").at(0).at("text").as_string()), &err)) out["value"] = *parsed;
    return out;
  }
  std::string get_as(const std::string& address) {
    return fetch(port, "localhost", bhttp::verb::get, "/ab", {{"x-forwarded-for", address}}).body;
  }
  static std::string text(const bj::object& r) { return std::string(r.at("text").as_string()); }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string live;
};

TEST_F(SplitsTest, AShareOfAddressesGetsTheCandidateAndStaysOnIt) {
  auto v2 = store(200, "two");
  auto started = op("split_start", {{"box", "ab"}, {"version", v2}, {"share", 50}, {"why", "try two"}});
  ASSERT_FALSE(started.at("isError").as_bool()) << text(started);
  EXPECT_EQ(started.at("value").at("mode"), "manual");
  int ones = 0, twos = 0;
  for (int i = 0; i < 200; ++i) {
    std::string addr = "203.0.113." + std::to_string(i % 250);
    auto body = get_as(addr);
    (body == "two" ? twos : ones)++;
    EXPECT_EQ(get_as(addr), body) << addr;  // the same address, the same version
  }
  EXPECT_GT(twos, 60);
  EXPECT_GT(ones, 60);
  auto rep = op("split_report", {{"box", "ab"}});
  ASSERT_FALSE(rep.at("isError").as_bool()) << text(rep);
  EXPECT_EQ(rep.at("value").at("state"), "splitting");
  EXPECT_EQ(rep.at("value").at("since_start").at("candidate").at("requests"), twos * 2);
  EXPECT_EQ(rep.at("value").at("since_start").at("live").at("requests"), ones * 2);
  // A share of 0 sends everyone back while the split stays.
  ASSERT_FALSE(op("split_set", {{"box", "ab"}, {"share", 0}, {"why", "pause"}}).at("isError").as_bool());
  for (int i = 0; i < 20; ++i) EXPECT_EQ(get_as("198.51.100." + std::to_string(i)), "one");
  // Rolled back: the live version answers everything, the candidate stays stored.
  auto stop = op("split_stop", {{"box", "ab"}, {"outcome", "rollback"}, {"why", "done"}});
  ASSERT_FALSE(stop.at("isError").as_bool()) << text(stop);
  EXPECT_EQ(stop.at("value").at("state"), "none");
  EXPECT_EQ(stop.at("value").at("last").at("outcome"), "rollback");
  EXPECT_EQ(host->boxes().active()->boxes.at("ab")->version, live);
  EXPECT_FALSE(op("split_list", {}).at("value").as_array().size());
}

TEST_F(SplitsTest, TheLadderStepsAndPromotesWhileGuardrailsHold) {
  auto v2 = store(200, "two");
  auto started = op("split_start", {{"box", "ab"}, {"version", v2}, {"ladder", bj::array{50, 100}}, {"step_minutes", 1},
                                    {"min_requests", 5}, {"why", "ramp"}});
  ASSERT_FALSE(started.at("isError").as_bool()) << text(started);
  EXPECT_EQ(started.at("value").at("share_percent").to_number<double>(), 50);
  for (int i = 0; i < 60; ++i) get_as("192.0.2." + std::to_string(i));
  host->split_tick(base::now_unix_ms());  // not a minute yet
  EXPECT_TRUE(host->boxes().active()->splits.count("ab"));
  host->split_tick(base::now_unix_ms() + 61000);
  EXPECT_FALSE(host->boxes().active()->splits.count("ab"));
  EXPECT_EQ(host->boxes().active()->boxes.at("ab")->version, v2);
  EXPECT_EQ(op("split_report", {{"box", "ab"}}).at("value").at("last").at("outcome"), "promote");
}

TEST_F(SplitsTest, AGuardrailRollsBackOnItsOwn) {
  auto v2 = store(500, "broken");
  ASSERT_FALSE(op("split_start", {{"box", "ab"}, {"version", v2}, {"share", 50}, {"why", "risky"}}).at("isError").as_bool());
  for (int i = 0; i < 120; ++i) get_as("192.0.2." + std::to_string(i));
  host->split_tick(base::now_unix_ms());
  EXPECT_FALSE(host->boxes().active()->splits.count("ab"));
  auto last = op("split_report", {{"box", "ab"}}).at("value").at("last");
  EXPECT_EQ(last.at("outcome"), "rollback");
  EXPECT_NE(std::string(last.at("end_why").as_string()).find("5xx rate"), std::string::npos) << json::dump(last);
  for (int i = 0; i < 20; ++i) EXPECT_EQ(get_as("192.0.2." + std::to_string(i)), "one");
}

TEST_F(SplitsTest, RefusesWhatCantSplit) {
  std::string err;
  auto moved = host->boxes().store(R"({"box":"ab","domains":["*"],"routes":{"http":{"/moved":{"get":{"name":"ab","public":true}}}}})",
                                   module_saying(200, "x"), "", &err);
  ASSERT_TRUE(moved) << err;
  auto r = op("split_start", {{"box", "ab"}, {"version", moved->version}, {"share", 5}, {"why", "x"}});
  ASSERT_TRUE(r.at("isError").as_bool());
  EXPECT_NE(text(r).find("can't split"), std::string::npos) << text(r);
  EXPECT_TRUE(op("split_start", {{"box", "ab"}, {"version", live}, {"share", 5}, {"why", "x"}}).at("isError").as_bool());
  EXPECT_TRUE(op("split_start", {{"box", "ab"}, {"version", store(200, "two")}, {"share", 101}, {"why", "x"}}).at("isError").as_bool());
  EXPECT_TRUE(op("split_start", {{"box", "ab"}, {"version", "x"}, {"ladder", bj::array{10, 5}}, {"why", "x"}}).at("isError").as_bool());
  EXPECT_TRUE(op("split_stop", {{"box", "ab"}, {"outcome", "rollback"}}).at("isError").as_bool());
  for (const char* t : {"split_start", "split_set", "split_stop", "split_report", "split_list"}) {
    EXPECT_TRUE(host::Host::scope_rule(t).has_value()) << t;
  }
  EXPECT_TRUE(host::Host::is_read_tool("split_report"));
  EXPECT_FALSE(host::Host::is_read_call("split_start", {}));
}
