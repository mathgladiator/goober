// Beta (design/machine/053.Beta.md): a beta domain linked to its production domain, in its own
// tenant, answered by the boxes' beta versions while production keeps the live ones.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

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

std::string site_manifest(const std::string& extra_route = "") {
  return R"({"box":"site","domains":["example.com"],"routes":{"http":{"/hi":{"get":{"name":"hi","public":true}})" + extra_route + "}}}";
}

std::string module_saying(const std::string& text, const std::vector<std::string>& routes = {"hi"}) {
  std::string wat = R"((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "text/plain")
  (data (i32.const 32) ")" + text + R"("))";
  for (const auto& r : routes) {
    wat += "\n  (func (export \"goober_route_" + r + "\") (result i32) (drop (call $respond (i32.const 200) (i32.const 0) "
           "(i32.const 10) (i32.const 32) (i32.const " + std::to_string(text.size()) + "))) (i32.const 0))";
  }
  return wat + ")";
}

const char* kToken = "op-token-for-beta-tests-0123456789";

}  // namespace

class BetaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "beta";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"example.com": {"tenant": "acme"}, "beta.example.com": {"tenant": "acme_beta", "beta-of": "example.com"}},
        "operations": {"token": "op-token-for-beta-tests-0123456789", "beta-domain": "beta-ops.example.com"},
        "threads": {"network": 1, "boxes": 2}})", root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    auto v1 = host->boxes().upload(site_manifest(), module_saying("live"), &err);
    ASSERT_TRUE(v1) << err;
    live = v1->version;
  }
  void TearDown() override { host.reset(); }
  bj::object op(const std::string& tool, const bj::object& args) {
    auto r = fetch(port, "example.com", bhttp::verb::post, "/goober-operations-mcp",
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
  static std::string text(const bj::object& r) { return std::string(r.at("text").as_string()); }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string live;
};

TEST_F(BetaTest, BetaAnswersWithTheLiveVersionUntilABetaVersionIsStaged) {
  EXPECT_EQ(fetch(port, "beta.example.com", bhttp::verb::get, "/hi").body, "live");
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "live");

  // A beta version that changes /hi and adds /new, staged with channel "beta".
  auto up = op("version_upload", {{"manifest", site_manifest(R"(,"/new":{"get":{"name":"fresh","public":true}})")},
                                  {"wat", module_saying("beta", {"hi", "fresh"})},
                                  {"channel", "beta"}});
  ASSERT_FALSE(up.at("isError").as_bool()) << text(up);
  EXPECT_TRUE(up.at("value").at("beta").as_bool());
  EXPECT_EQ(fetch(port, "beta.example.com", bhttp::verb::get, "/hi").body, "beta");
  EXPECT_EQ(fetch(port, "beta.example.com", bhttp::verb::get, "/new").body, "beta");
  // Production is untouched.
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "live");
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/new").status, 404);
  EXPECT_EQ(host->boxes().active()->boxes.at("site")->version, live);

  auto st = op("beta_status", {});
  ASSERT_FALSE(st.at("isError").as_bool()) << text(st);
  EXPECT_EQ(st.at("value").at("sites").at(0).at("tenant"), "acme_beta");
  EXPECT_EQ(st.at("value").at("boxes").at(0).at("state"), "differs from live");
  EXPECT_EQ(st.at("value").at("beta_ops"), "beta-ops.example.com");

  // Cleared, beta follows live again.
  ASSERT_FALSE(op("beta_clear", {{"box", "site"}, {"why", "done"}}).at("isError").as_bool());
  EXPECT_EQ(fetch(port, "beta.example.com", bhttp::verb::get, "/hi").body, "live");
  // version_promote with channel beta stages a stored version.
  std::string err;
  auto stored = host->boxes().store(site_manifest(), module_saying("again"), "", &err);
  ASSERT_TRUE(stored) << err;
  auto promoted = op("version_promote", {{"box", "site"}, {"version", stored->version}, {"channel", "beta"}});
  ASSERT_FALSE(promoted.at("isError").as_bool()) << text(promoted);
  EXPECT_EQ(promoted.at("value").at("beta").as_string(), stored->version) << text(promoted);
  EXPECT_EQ(fetch(port, "beta.example.com", bhttp::verb::get, "/hi").body, "again");
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "live");
}

TEST_F(BetaTest, ABoxOnlyInBetaAnswersOnlyThere) {
  auto up = op("version_upload", {{"manifest", R"({"box":"fresh","domains":["example.com"],"routes":{"http":{"/fresh":{"get":{"name":"fresh_get","public":true}}}}})"},
                                  {"wat", module_saying("fresh", {"fresh_get"})},
                                  {"channel", "beta"}});
  ASSERT_FALSE(up.at("isError").as_bool()) << text(up);
  EXPECT_EQ(fetch(port, "beta.example.com", bhttp::verb::get, "/fresh").body, "fresh");
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/fresh").status, 404);
  bool found = false;
  auto st = op("beta_status", {});
  for (const auto& b : st.at("value").at("boxes").as_array()) found |= b.at("box") == "fresh" && b.at("state") == "beta only";
  EXPECT_TRUE(found);
  EXPECT_TRUE(op("version_upload", {{"manifest", site_manifest()}, {"wat", module_saying("x")}, {"channel", "staging"}}).at("isError").as_bool());
}
