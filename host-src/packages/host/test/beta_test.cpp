// Beta (design/machine/053.Beta.md): a beta domain linked to its production domain, in its own
// tenant, answered by the boxes' beta versions while production keeps the live ones.

#include <gtest/gtest.h>

#include <fstream>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/files.h"
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
        "domains": {"example.com": {"tenant": "acme"}, "beta.example.com": {"tenant": "acme_beta", "beta-of": "example.com"},
                    "other.example.com": {"tenant": "other"}},
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
  bj::object out{{"isError", res.at("isError")}, {"text", res.at("content").at(0).at("text")}};
  if (auto parsed = json::parse(std::string(res.at("content").at(0).at("text").as_string()), &err)) out["value"] = *parsed;
  return out;
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

TEST_F(BetaTest, APromotionCarriesBoxesAndContentAsOneChangeSet) {
  auto up = op("version_upload", {{"manifest", site_manifest()}, {"wat", module_saying("next")}, {"channel", "beta"}});
  ASSERT_FALSE(up.at("isError").as_bool()) << text(up);
  std::string next(up.at("value").at("version").as_string());
  ASSERT_FALSE(op("page_put", {{"tenant", "acme_beta"}, {"path", "/about"}, {"format", "markdown"}, {"body", "# About\n\nNew words."}})
                   .at("isError").as_bool());

  auto plan = op("promotion_preview", {{"tenant", "acme"}});
  ASSERT_FALSE(plan.at("isError").as_bool()) << text(plan);
  ASSERT_EQ(plan.at("value").at("boxes").as_array().size(), 1u) << text(plan);
  EXPECT_EQ(plan.at("value").at("boxes").at(0).at("to").as_string(), next);
  EXPECT_EQ(plan.at("value").at("boxes").at(0).at("from").as_string(), live);
  ASSERT_GE(plan.at("value").at("content").as_array().size(), 1u) << text(plan);
  EXPECT_EQ(plan.at("value").at("content").at(0).at("change"), "new");
  EXPECT_TRUE(op("promotion_preview", {{"tenant", "acme_beta"}}).at("isError").as_bool());  // a beta has no beta

  auto started = op("promotion_start", {{"tenant", "acme"}, {"why", "ship the new hello"}});
  ASSERT_FALSE(started.at("isError").as_bool()) << text(started);
  std::string cs(started.at("value").at("changeset").as_string());
  // Nothing is live until it applies.
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "live");
  bj::object apply{{"id", cs}};
  if (started.at("value").at("draft_version").is_int64()) apply["version"] = started.at("value").at("draft_version");
  auto applied = op("changeset_apply", apply);
  ASSERT_FALSE(applied.at("isError").as_bool()) << text(applied);
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "next");
  EXPECT_NE(fetch(port, "example.com", bhttp::verb::get, "/about").body.find("New words"), std::string::npos);
  // The beta slot cleared itself: beta follows live, now the same.
  EXPECT_FALSE(host->boxes().beta_version("site"));
  EXPECT_EQ(fetch(port, "beta.example.com", bhttp::verb::get, "/hi").body, "next");
  // Rolled back in one call.
  auto back = op("changeset_rollback", {{"id", cs}});
  ASSERT_FALSE(back.at("isError").as_bool()) << text(back);
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "live");
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/about").status, 404);
  // Beta's operator can't promote.
  auto refused = call_on(port, "beta-ops.example.com", kToken, "promotion_start", {{"tenant", "acme"}, {"why", "x"}});
  EXPECT_TRUE(refused.at("isError").as_bool()) << refused.at("text");
}

// Out of development mode: what beta's operator may drop at once, and what waits for an owner.
TEST(BetaHeld, BetasOperatorDropsOnlyWhatIsBetasAloneAndKeepsItsApprovals) {
  auto root = fs::path(GOOBER_TEST_TMP) / "beta_held";
  fs::remove_all(root);
  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"example.com": {"tenant": "acme"}, "beta.example.com": {"tenant": "acme_beta", "beta-of": "example.com"}},
      "operations": {"token": "op-token-for-beta-tests-0123456789", "beta-domain": "beta-ops.example.com"},
      "threads": {"network": 1, "boxes": 2}})", root, &err);
  ASSERT_TRUE(cfg) << err;
  auto prod = std::make_shared<config::HostConfig>(*config::local_mode(cfg));
  prod->local = false;
  auto host = std::make_unique<host::Host>(prod);
  uint16_t port = host->start();
  const std::string ops = "beta-ops.example.com";
  auto beta = [&](const std::string& tool, const bj::object& args) { return call_on(port, ops, kToken, tool, args); };
  auto pending = [](const bj::object& r) {
    const auto* v = r.if_contains("value");
    return v && v->is_object() && v->as_object().contains("pending_approval");
  };

  // Production's box, disabled: beta's drop of it waits for an owner and drops nothing.
  ASSERT_TRUE(host->boxes().upload(site_manifest(), module_saying("live"), &err)) << err;
  ASSERT_TRUE(host->boxes().disable("site", "paused", &err)) << err;
  auto held = beta("box_drop", {{"box", "site"}, {"why", "tidy"}});
  EXPECT_TRUE(pending(held)) << held.at("text");
  EXPECT_FALSE(host->boxes().versions("site").empty());
  // Production's dark version staged for beta is still not beta's alone.
  auto staged = beta("version_promote", {{"box", "site"}, {"version", host->boxes().versions("site").back().version},
                                        {"channel", "beta"}});
  ASSERT_FALSE(staged.at("isError").as_bool()) << staged.at("text");
  EXPECT_FALSE(host->beta_only_box("site"));

  // A box only ever in beta drops at once.
  auto fresh = beta("version_upload", {{"manifest", R"({"box":"fresh","domains":["example.com"],"routes":{"http":{"/f":{"get":{"name":"f","public":true}}}}})"},
                                       {"wat", module_saying("f", {"f"})},
                                       {"channel", "beta"}});
  ASSERT_FALSE(fresh.at("isError").as_bool()) << fresh.at("text");
  EXPECT_TRUE(host->beta_only_box("fresh"));
  auto dropped = beta("box_drop", {{"box", "fresh"}, {"why", "tidy"}});
  ASSERT_FALSE(dropped.at("isError").as_bool()) << dropped.at("text");
  EXPECT_FALSE(pending(dropped)) << dropped.at("text");
  EXPECT_TRUE(host->boxes().dropped("fresh"));

  // The approvals switch an owner sets on an agent holds it through beta-ops too.
  host::SignedIn owner;
  owner.user.email = "boss@example.com";
  owner.roles = {"host:owner"};
  ASSERT_EQ(host->panel_set_approvals(owner, {{"confirm", true}, {"agent", "config"}, {"on", true}}, "t").first, 200);
  auto put = beta("page_put", {{"tenant", "acme_beta"}, {"path", "/about"}, {"format", "markdown"}, {"body", "x"}});
  EXPECT_TRUE(pending(put)) << put.at("text");
  host.reset();
}

TEST_F(BetaTest, InjectedMailGoesOnlyIntoABetaInTheOperatorsFocus) {
  std::string err;
  auto grok = *host::create_operator_token(root / "storage", "grok", 0, &err);
  host::Host::OperatorScope production;
  production.all = false;
  production.tenants = {"acme"};
  ASSERT_TRUE(host->set_operator_scope("token:grok", production, "test", &err)) << err;
  auto r = call_on(port, "example.com", grok, "beta_mail_send",
                   {{"to", "support@beta.example.com"}, {"from", "ann@example.org"}, {"subject", "s"}, {"text", "t"}});
  EXPECT_TRUE(r.at("isError").as_bool()) << r.at("text");
  EXPECT_NE(std::string(r.at("text").as_string()).find("isn't focused on acme_beta"), std::string::npos) << r.at("text");
}

TEST_F(BetaTest, BetasOperatorReadsOnlyBetasBoxLogLines) {
  auto file = root / "storage" / "logs" / "boxes" / "site.log";
  fs::create_directories(file.parent_path());
  {
    std::ofstream out(file, std::ios::app);
    out << R"({"tenant":"acme","level":"info","text":"production's"})" << "\n"
        << R"({"tenant":"acme_beta","level":"info","text":"beta's"})" << "\n";
  }
  auto lines = call_on(port, "beta-ops.example.com", kToken, "box_logs", {{"box", "site"}});
  ASSERT_FALSE(lines.at("isError").as_bool()) << lines.at("text");
  std::string text(lines.at("text").as_string());
  EXPECT_NE(text.find("beta's"), std::string::npos) << text;
  EXPECT_EQ(text.find("production's"), std::string::npos) << text;
  // The owner's token on production's domain reads both.
  auto all = op("box_logs", {{"box", "site"}});
  EXPECT_NE(BetaTest::text(all).find("production's"), std::string::npos);
}

TEST_F(BetaTest, ADropCheckCountsWhatBetaVersionsBind) {
  std::string err;
  ASSERT_TRUE(host->schemas().put(R"({"schema":"items1","tables":{"items":{"columns":{"name":{"type":"string","max-length":40}}}}})", &err)) << err;
  auto schema = host->schemas().get("items1");
  ASSERT_TRUE(host->databases().open("acme_beta", "items", &*schema, &err)) << err;
  // A box only in beta, its strategy pinned to acme: on beta it opens acme_beta's items.
  auto up = op("version_upload",
               {{"manifest", R"({"box":"keeper","domains":["example.com"],
                   "data":{"sqlite3":{"w":{"schema":"items1","database":"items","tenant":"acme","pattern":"sql","tables":{"items":{"read":true}}}}},
                   "routes":{"http":{"/k":{"get":{"name":"k","public":true}}}}})"},
                {"wat", module_saying("k", {"k"})},
                {"channel", "beta"}});
  ASSERT_FALSE(up.at("isError").as_bool()) << text(up);
  auto check = op("db_drop", {{"tenant", "acme_beta"}, {"database", "items"}, {"dry_run", true}});
  ASSERT_FALSE(check.at("isError").as_bool()) << text(check);
  EXPECT_NE(text(check).find("keeper"), std::string::npos) << text(check);
  auto box = op("box_drop", {{"box", "keeper"}, {"dry_run", true}});
  EXPECT_EQ(box.at("value").at("state"), "beta") << text(box);
}

TEST(BetaMailbox, KeepsWithinItsBytesAndListsFromRows) {
  auto root = fs::path(GOOBER_TEST_TMP) / "beta_mailbox";
  fs::remove_all(root);
  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"example.com": {"tenant": "acme"}, "beta.example.com": {"tenant": "acme_beta", "beta-of": "example.com"}},
      "limits": {"beta-mail-bytes": 1048576}, "threads": {"network": 1, "boxes": 2}})", root, &err);
  ASSERT_TRUE(cfg) << err;
  auto host = std::make_unique<host::Host>(config::local_mode(cfg));
  std::string big(300 * 1024, 'x');
  std::vector<std::string> ids;
  for (int i = 0; i < 6; ++i) {
    ids.push_back(host->beta_mail_keep("acme_beta", bj::object{{"direction", "push"}, {"from", "box"}, {"to", "push"},
                                                              {"subject", "s" + std::to_string(i)}, {"text", big}, {"html", ""}}));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // Each cut at 256 KiB, said so; the oldest gone past 1 MiB, the newest kept.
  auto newest = host->beta_mail_get("acme_beta", ids.back());
  ASSERT_TRUE(newest);
  EXPECT_EQ(newest->at("text").as_string().size(), 256u * 1024);
  EXPECT_TRUE(newest->at("truncated").as_bool());
  EXPECT_FALSE(host->beta_mail_get("acme_beta", ids.front()));
  auto list = host->beta_mail_list("acme_beta", 100);
  EXPECT_GE(list.size(), 3u);
  EXPECT_LE(list.size(), 4u);
  EXPECT_EQ(list.at(0).at("subject"), "s5");
  EXPECT_EQ(list.at(0).at("preview").as_string().size(), 160u + std::string("…").size());
}

TEST_F(BetaTest, CutMessagesStayText) {
  // A cut that lands inside a character steps back to its start, so the message reads back.
  std::string text = std::string(256 * 1024 - 1, 'a') + "é and more";
  std::string head = std::string(159, 'b') + "ü tail";
  auto id = host->beta_mail_keep("acme_beta", bj::object{{"direction", "out"}, {"from", "a@example.com"}, {"to", "b@example.com"},
                                                        {"subject", "s"}, {"text", text}, {"html", ""}});
  auto got = host->beta_mail_get("acme_beta", id);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->at("text").as_string().size(), 256u * 1024 - 1);
  auto row_id = host->beta_mail_keep("acme_beta", bj::object{{"direction", "out"}, {"from", "a@example.com"}, {"to", "b@example.com"},
                                                            {"subject", "short"}, {"text", head}, {"html", ""}});
  auto row = base::read_file(root / "storage/mail/beta/acme_beta" / (row_id + ".head"));
  ASSERT_TRUE(row);
  std::string err;
  EXPECT_TRUE(json::parse(*row, &err)) << err;
  EXPECT_EQ(host->beta_mail_list("acme_beta", 10).size(), 2u);
}

TEST_F(BetaTest, BetaStatusShowsAFocusedOperatorItsOwn) {
  std::string err;
  auto grok = *host::create_operator_token(root / "storage", "grok", 0, &err);
  host::Host::OperatorScope elsewhere;
  elsewhere.all = false;
  elsewhere.tenants = {"other"};
  ASSERT_TRUE(host->set_operator_scope("token:grok", elsewhere, "test", &err)) << err;
  auto own = op("beta_status", {});
  EXPECT_NE(text(own).find("\"site\""), std::string::npos) << text(own);
  auto theirs = call_on(port, "example.com", grok, "beta_status", {});
  EXPECT_EQ(std::string(theirs.at("text").as_string()).find("\"site\""), std::string::npos) << theirs.at("text");
  // The limits check counts beta's mailboxes.
  EXPECT_NE(text(op("host_health", {})).find("beta-mail-bytes"), std::string::npos);
}

TEST_F(BetaTest, OnlySourceDeployTakesAChannel) {
  auto r = fetch(port, "example.com", bhttp::verb::post, "/goober-operations-mcp",
                 {{"authorization", std::string("Bearer ") + kToken}, {"content-type", "application/json"}},
                 R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
  std::string err;
  auto v = json::parse(r.body, &err);
  ASSERT_TRUE(v) << err;
  int seen = 0;
  for (const auto& t : v->at("result").at("tools").as_array()) {
    const auto& name = t.at("name").as_string();
    if (name != "source_build" && name != "source_deploy") continue;
    ++seen;
    EXPECT_EQ(t.at("inputSchema").at("properties").as_object().contains("channel"), name == "source_deploy") << name;
  }
  EXPECT_EQ(seen, 2);
}

// A promotion request (053 § 11): beta's operator asks; it waits for an owner even in
// development mode; the owner approves by typing the tenant, and production gets beta's version.
TEST_F(BetaTest, BetasOperatorAsksForAPromotionAndTheOwnerApprovesIt) {
  const std::string ops = "beta-ops.example.com";
  auto staged = call_on(port, ops, kToken, "version_upload", {{"manifest", site_manifest()}, {"wat", module_saying("beta")}, {"channel", "beta"}});
  ASSERT_FALSE(staged.at("isError").as_bool()) << staged.at("text");
  auto asked = call_on(port, ops, kToken, "promotion_request", {{"tenant", "acme"}, {"why", "beta says beta"}});
  ASSERT_FALSE(asked.at("isError").as_bool()) << asked.at("text");
  ASSERT_TRUE(asked.at("value").as_object().contains("pending_approval")) << asked.at("text");
  int64_t id = asked.at("value").at("pending_approval").as_int64();
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "live");

  // The owner's panel sees what it pinned.
  host::SignedIn owner;
  owner.user.email = "boss@example.com";
  owner.roles = {"host:owner"};
  auto pending = host->panel_approvals(owner).at("pending").as_array();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].at("promotion").at("pinned").at("boxes").at("site").as_string(), host->boxes().beta_version("site").value_or(""));
  EXPECT_FALSE(pending[0].at("promotion").at("beta_moved").as_bool());

  // Only an owner approves, typing the tenant.
  host::SignedIn operator_;
  operator_.user.email = "sam@example.com";
  operator_.roles = {"host:operator"};
  EXPECT_EQ(host->panel_decide(operator_, {{"id", id}, {"decision", "approve"}, {"confirm", true}, {"confirm_name", "acme"}}, "t").first, 403);
  EXPECT_EQ(host->panel_decide(owner, {{"id", id}, {"decision", "approve"}, {"confirm", true}, {"confirm_name", "acm"}}, "t").first, 400);
  auto done = host->panel_decide(owner, {{"id", id}, {"decision", "approve"}, {"confirm", true}, {"confirm_name", "acme"}}, "t");
  ASSERT_EQ(done.first, 200) << json::dump(done.second);
  EXPECT_FALSE(done.second.at("is_error").as_bool()) << json::dump(done.second);
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "beta");
}

TEST_F(BetaTest, APromotionRequestIsRefusedWhenBetaMovedAndOutsideTheFocus) {
  const std::string ops = "beta-ops.example.com";
  ASSERT_FALSE(call_on(port, ops, kToken, "version_upload", {{"manifest", site_manifest()}, {"wat", module_saying("one")}, {"channel", "beta"}})
                   .at("isError").as_bool());
  auto asked = call_on(port, ops, kToken, "promotion_request", {{"tenant", "acme"}, {"why", "one"}});
  int64_t id = asked.at("value").at("pending_approval").as_int64();
  ASSERT_FALSE(call_on(port, ops, kToken, "version_upload", {{"manifest", site_manifest()}, {"wat", module_saying("two")}, {"channel", "beta"}})
                   .at("isError").as_bool());
  host::SignedIn owner;
  owner.user.email = "boss@example.com";
  owner.roles = {"host:owner"};
  EXPECT_TRUE(host->panel_approvals(owner).at("pending").at(0).at("promotion").at("beta_moved").as_bool());
  auto done = host->panel_decide(owner, {{"id", id}, {"decision", "approve"}, {"confirm", true}, {"confirm_name", "acme"}}, "t");
  ASSERT_EQ(done.first, 200);
  EXPECT_TRUE(done.second.at("is_error").as_bool());
  EXPECT_NE(json::dump(done.second).find("beta changed since the request"), std::string::npos) << json::dump(done.second);
  EXPECT_EQ(fetch(port, "example.com", bhttp::verb::get, "/hi").body, "live");

  // An operator focused on another site can't ask for acme's.
  std::string err;
  auto grok = *host::create_operator_token(root / "storage", "grok", 0, &err);
  host::Host::OperatorScope other;
  other.all = false;
  other.tenants = {"other"};
  ASSERT_TRUE(host->set_operator_scope("token:grok", other, "test", &err)) << err;
  auto refused = call_on(port, "example.com", grok, "promotion_request", {{"tenant", "acme"}, {"why", "x"}});
  EXPECT_TRUE(refused.at("isError").as_bool()) << refused.at("text");
}

TEST_F(BetaTest, BetasMailboxEmpties) {
  for (int i = 0; i < 3; ++i) host->beta_mail_keep("acme_beta", bj::object{{"direction", "out"}, {"from", "a@x.example"}, {"to", "b@x.example"},
                                                                         {"subject", "s" + std::to_string(i)}, {"text", "t"}});
  auto list = host->beta_mail_list("acme_beta", 10);
  ASSERT_EQ(list.size(), 3u);
  auto one = call_on(port, "beta-ops.example.com", kToken, "beta_mail_clear", {{"tenant", "acme_beta"}, {"ids", bj::array{list[0].at("id")}}});
  ASSERT_FALSE(one.at("isError").as_bool()) << one.at("text");
  EXPECT_EQ(one.at("value").at("removed"), 1);
  EXPECT_EQ(one.at("value").at("left"), 2);
  auto all = op("beta_mail_clear", {{"tenant", "acme_beta"}});
  EXPECT_EQ(all.at("value").at("removed"), 2);
  EXPECT_TRUE(host->beta_mail_list("acme_beta", 10).empty());
  EXPECT_TRUE(op("beta_mail_clear", {{"tenant", "acme"}}).at("isError").as_bool());  // production has no mailbox
}

// Seeding (053 § 13): production's database copied over beta's, beta's own snapshotted first;
// beta's operator can't seed.
TEST_F(BetaTest, BetaIsSeededFromProduction) {
  std::string err;
  ASSERT_TRUE(host->schemas().put(R"({"schema":"notes1","tables":{"notes":{"columns":{"body":{"type":"string"}}}}})", &err)) << err;
  auto schema = host->schemas().get("notes1");
  ASSERT_TRUE(host->databases().ensure_schema("acme", "notes", *schema, &err)) << err;
  ASSERT_TRUE(host->databases().ensure_schema("acme_beta", "notes", *schema, &err)) << err;
  ASSERT_TRUE(host->databases().operate("acme", "notes", "INSERT INTO notes (body) VALUES ('real')", nullptr, 10, true, &err)) << err;
  ASSERT_TRUE(host->databases().operate("acme_beta", "notes", "INSERT INTO notes (body) VALUES ('test')", nullptr, 10, true, &err)) << err;
  ASSERT_TRUE(host->content().put("acme", "index.html", "production", &err)) << err;

  auto dry = op("beta_seed", {{"tenant", "acme"}, {"content", true}, {"dry_run", true}});
  ASSERT_FALSE(dry.at("isError").as_bool()) << text(dry);
  EXPECT_TRUE(dry.at("value").at("databases").at(0).at("replaces_beta").as_bool());
  EXPECT_EQ(dry.at("value").at("content").at(0), "index.html");
  EXPECT_FALSE(host->content().get("acme_beta", "index.html"));

  EXPECT_TRUE(call_on(port, "beta-ops.example.com", kToken, "beta_seed", {{"tenant", "acme"}}).at("isError").as_bool());
  EXPECT_TRUE(op("beta_seed", {{"tenant", "acme_beta"}}).at("isError").as_bool());

  auto seeded = op("beta_seed", {{"tenant", "acme"}, {"content", true}, {"why", "try on real data"}});
  ASSERT_FALSE(seeded.at("isError").as_bool()) << text(seeded);
  auto rows = host->databases().operate("acme_beta", "notes", "SELECT body FROM notes", nullptr, 10, false, &err);
  ASSERT_TRUE(rows) << err;
  ASSERT_EQ(rows->at("rows").as_array().size(), 1u);
  EXPECT_EQ(rows->at("rows").at(0).at(0), "real");
  EXPECT_EQ(host->content().get("acme_beta", "index.html")->data, "production");
  // Beta's own was kept as a snapshot first.
  std::string snap(seeded.at("value").at("snapshots").at("notes").as_string());
  bool found = false;
  auto snaps = op("db_snapshots", {{"tenant", "acme_beta"}});
  for (const auto& s : snaps.at("value").at("snapshots").as_array()) found |= s.at("snapshot").as_string() == snap;
  EXPECT_TRUE(found) << snap;
}
