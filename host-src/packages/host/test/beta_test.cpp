// Beta (design/machine/053.Beta.md): a beta domain linked to its production domain, in its own
// tenant, answered by the boxes' beta versions while production keeps the live ones.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"
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

TEST_F(BetaTest, BetaMailIsCapturedAndInjectedMailReachesTheBox) {
  // A sign-in on beta: its code is captured in beta's mailbox, never sent.
  auto start = fetch(port, "beta.example.com", bhttp::verb::post, "/identity/email/start",
                     {{"content-type", "application/x-www-form-urlencoded"}}, "email=tester@example.org");
  EXPECT_LT(start.status, 500) << start.body;
  host->drain_mail();
  auto list = op("beta_mail_list", {{"tenant", "acme_beta"}});
  ASSERT_FALSE(list.at("isError").as_bool()) << text(list);
  ASSERT_EQ(list.at("value").as_array().size(), 1u) << text(list);
  const auto& code = list.at("value").at(0);
  EXPECT_EQ(code.at("direction"), "out");
  EXPECT_EQ(code.at("to"), "tester@example.org");
  std::error_code ec;
  EXPECT_TRUE(!fs::exists(root / "storage/mail/outbox", ec) || fs::is_empty(root / "storage/mail/outbox", ec));
  auto got = op("beta_mail_get", {{"tenant", "acme_beta"}, {"id", code.at("id")}});
  ASSERT_FALSE(got.at("isError").as_bool()) << text(got);
  EXPECT_FALSE(std::string(got.at("value").at("text").as_string()).empty());
  EXPECT_TRUE(op("beta_mail_list", {{"tenant", "acme"}}).at("isError").as_bool());  // production has no mailbox
  EXPECT_TRUE(op("beta_mail_get", {{"tenant", "acme_beta"}, {"id", "../../keys"}}).at("isError").as_bool());

  // A box with a mail route, staged for beta: injected mail reaches it, and the outcome is kept.
  std::string mail_manifest = R"({"box":"desk","domains":["example.com"],"routes":{"mail":{"support@":{"receive":{"name":"support_inbox","public":true}}}}})";
  auto up = op("version_upload", {{"manifest", mail_manifest}, {"wat", module_saying("ok", {"support_inbox"})}, {"channel", "beta"}});
  ASSERT_FALSE(up.at("isError").as_bool()) << text(up);
  auto sent = op("beta_mail_send", {{"to", "support@beta.example.com"}, {"from", "ann@example.org"}, {"subject", "Help"},
                                    {"text", "My order is late."}, {"in_reply_to", code.at("id")}});
  ASSERT_FALSE(sent.at("isError").as_bool()) << text(sent);
  EXPECT_EQ(sent.at("value").at("outcome"), "delivered") << text(sent);
  EXPECT_EQ(sent.at("value").at("box"), "desk");
  auto in = op("beta_mail_get", {{"tenant", "acme_beta"}, {"id", sent.at("value").at("id")}});
  EXPECT_EQ(in.at("value").at("direction"), "in");
  EXPECT_EQ(in.at("value").at("in_reply_to"), got.at("value").at("message_id"));
  // Only beta takes injected mail.
  auto prod = op("beta_mail_send", {{"to", "support@example.com"}, {"from", "ann@example.org"}, {"subject", "x"}, {"text", "x"}});
  ASSERT_TRUE(prod.at("isError").as_bool());
  EXPECT_NE(text(prod).find("isn't a beta domain"), std::string::npos) << text(prod);
}

namespace {

// ask: calls svc.lookup and answers what came back.
const char* kAskWat = R"wat((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "svc")
  (data (i32.const 20) "lookup")
  (data (i32.const 48) "{}")
  (func (export "goober_route_ask") (result i32)
    (local $n i32)
    (local.set $n (call $call (i32.const 16) (i32.const 3) (i32.const 20) (i32.const 6) (i32.const 48) (i32.const 2)))
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 4096) (local.get $n)))
    (i32.const 0))))wat";

std::string ask_manifest(const std::string& beta) {
  return R"({"box":"asker","domains":["example.com"],
    "external":{"svc":{"base":"http://127.0.0.1:9",)" + beta + R"("methods":{"lookup":{"verb":"get","path":"/x"}}}},
    "routes":{"http":{"/ask":{"get":{"name":"ask","public":true}}}}})";
}

}  // namespace

TEST_F(BetaTest, ServicesFailInBetaUnlessTheyAllowReads) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(ask_manifest(""), kAskWat, &err)) << err;
  auto beta = fetch(port, "beta.example.com", bhttp::verb::get, "/ask").body;
  EXPECT_NE(beta.find("isn't reached from beta"), std::string::npos) << beta;
  // Production calls it (and fails to connect: there's no one at port 9), never refused for beta.
  auto prod = fetch(port, "example.com", bhttp::verb::get, "/ask").body;
  EXPECT_EQ(prod.find("beta"), std::string::npos) << prod;
  // "beta": "allow" lets a read-only method through from beta.
  ASSERT_TRUE(host->boxes().upload(ask_manifest(R"("beta":"allow",)"), kAskWat, &err)) << err;
  auto allowed = fetch(port, "beta.example.com", bhttp::verb::get, "/ask").body;
  EXPECT_EQ(allowed.find("isn't reached from beta"), std::string::npos) << allowed;
  EXPECT_FALSE(host->boxes().upload(ask_manifest(R"("beta":"sometimes",)"), kAskWat, &err));
}

namespace {

bj::object call_on(uint16_t port, const std::string& domain, const std::string& bearer, const std::string& tool, const bj::object& args) {
  auto r = fetch(port, domain, bhttp::verb::post, "/goober-operations-mcp",
                 {{"authorization", "Bearer " + bearer}, {"content-type", "application/json"}},
                 json::dump(bj::object{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                       {"params", bj::object{{"name", tool}, {"arguments", args}}}}));
  std::string err;
  auto v = json::parse(r.body, &err);
  if (!v || !v->as_object().contains("result")) return bj::object{{"isError", true}, {"text", r.body}};
  auto res = v->as_object().at("result").as_object();
  return bj::object{{"isError", res.at("isError")}, {"text", res.at("content").at(0).at("text")}};
}

}  // namespace

TEST_F(BetaTest, ThroughBetaOpsAnOperatorChangesOnlyBeta) {
  const std::string ops = "beta-ops.example.com";
  auto beta = [&](const std::string& tool, const bj::object& args) { return call_on(port, ops, kToken, tool, args); };
  auto refused = [&](const bj::object& r, const std::string& why) {
    EXPECT_TRUE(r.at("isError").as_bool()) << r.at("text");
    EXPECT_NE(std::string(r.at("text").as_string()).find(why), std::string::npos) << r.at("text");
  };
  // Production's box: staged for beta only.
  refused(beta("version_upload", {{"manifest", site_manifest()}, {"wat", module_saying("x")}}), "channel");
  auto staged = beta("version_upload", {{"manifest", site_manifest()}, {"wat", module_saying("staged")}, {"channel", "beta"}});
  ASSERT_FALSE(staged.at("isError").as_bool()) << staged.at("text");
  EXPECT_EQ(fetch(port, "beta.example.com", bhttp::verb::get, "/hi").body, "staged");
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "live");
  refused(beta("version_promote", {{"box", "site"}, {"version", live}}), "channel");
  refused(beta("box_disable", {{"box", "site"}, {"why", "x"}}), "changes production");
  refused(beta("box_drop", {{"box", "site"}, {"why", "x"}}), "live in production");
  // Content and data: beta's tenant, never production's.
  refused(beta("page_put", {{"tenant", "acme"}, {"path", "/x"}, {"format", "markdown"}, {"body", "x"}}), "");
  auto page = beta("page_put", {{"tenant", "acme_beta"}, {"path", "/about"}, {"format", "markdown"}, {"body", "beta about"}});
  EXPECT_FALSE(page.at("isError").as_bool()) << page.at("text");
  auto bs = beta("beta_status", {});
  EXPECT_FALSE(bs.at("isError").as_bool()) << bs.at("text");
  refused(beta("backup_status", {}), "");
  // The same token on production's domain is the owner's, as always.
  EXPECT_FALSE(call_on(port, "example.com", kToken, "version_promote", {{"box", "site"}, {"version", live}}).at("isError").as_bool());

  // A token focused on beta tenants alone is beta's anywhere.
  std::string err;
  auto grok = *host::create_operator_token(root / "storage", "grok", 0, &err);
  host::Host::OperatorScope only_beta;
  only_beta.all = false;
  only_beta.tenants = {"acme_beta"};
  ASSERT_TRUE(host->set_operator_scope("token:grok", only_beta, "test", &err)) << err;
  EXPECT_TRUE(host->operator_scope("token:grok").beta);
  refused(call_on(port, "example.com", grok, "version_upload", {{"manifest", site_manifest()}, {"wat", module_saying("x")}}), "channel");
  EXPECT_FALSE(host->operator_scope("token:other").beta);
  EXPECT_TRUE(host->operator_scope("beta:token:other").beta);
  EXPECT_EQ(host->operator_scope("beta:token:other").tenants, std::set<std::string>{"acme_beta"});
}
