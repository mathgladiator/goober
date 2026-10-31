// Operator scope (design/machine/030.OperatorScope.md): an operator focused on some tenants
// reaches only them, over MCP and in the panel, and only a human changes it, in the panel.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/sqlite/sqlite.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"
#include "../src/keys.h"
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
  std::map<std::string, std::string> headers;
};

const std::string kOps = "ops.example.com";

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
  Reply r{int(res.result_int()), res.body(), {}};
  for (const auto& f : res) r.headers[base::to_lower(std::string(f.name_string()))] = std::string(f.value());
  return r;
}

bj::value parse(const std::string& s) {
  std::string err;
  auto v = json::parse(s, &err);
  return v ? *v : bj::value(nullptr);
}

const std::vector<std::pair<std::string, std::string>> kForm = {{"content-type", "application/x-www-form-urlencoded"}};
const char* kModule = R"((module (memory (export "memory") 1) (func (export "goober_route_hi") (result i32) (i32.const 0))))";

}  // namespace

class ScopeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "operator_scope";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"kc.example.com": {"tenant": "kc"}, "other.example.com": {"tenant": "other"},
                    "localhost": {"tenant": "kc"}},
        "operations": {"domain": "ops.example.com"},
        "identity": {"operators": {"boss@example.com": ["host:owner"], "helper@example.com": ["host:operator"],
                                   "watcher@example.com": ["host:observer"]}},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    token = *host::create_operator_token(root / "storage", "grok", 0, &err);
    ASSERT_TRUE(host->boxes().upload(
        R"({"box":"kcbox","domains":["kc.example.com"],"routes":{"http":{"/hi":{"get":{"name":"hi","public":true}}}}})",
        kModule, &err))
        << err;
    ASSERT_TRUE(host->boxes().upload(
        R"({"box":"everywhere","domains":["*"],"routes":{"http":{"/all":{"get":{"name":"hi2","public":true}}}}})",
        std::regex_replace(kModule, std::regex("route_hi"), "route_hi2"), &err))
        << err;
  }
  void TearDown() override { host.reset(); }

  std::string sign_in(const std::string& email) {
    fetch(port, kOps, bhttp::verb::post, "/identity/email/start", kForm, "email=" + email);
    host->drain_mail();
    auto files = base::list_files(root / "storage/mail/outbox");
    std::sort(files.begin(), files.end());
    std::string code;
    for (auto it = files.rbegin(); it != files.rend() && code.empty(); ++it) {
      auto text = *base::read_file(root / "storage/mail/outbox" / *it);
      std::smatch m;
      if (text.find("To: " + email) != std::string::npos && std::regex_search(text, m, std::regex("is ([0-9]{6})"))) code = m[1];
    }
    auto v = fetch(port, kOps, bhttp::verb::post, "/identity/email/verify", kForm, "email=" + email + "&code=" + code);
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }
  Reply api(const std::string& cookie, const std::string& tool, const std::string& args = "{}") {
    return fetch(port, kOps, bhttp::verb::post, "/api/" + tool,
                 {{"cookie", cookie}, {"content-type", "application/json"}, {"origin", "http://" + kOps}}, args);
  }
  // A tool call over MCP: the result object, with isError and the text.
  bj::object call(const std::string& tool, const std::string& args = "{}") {
    auto r = fetch(port, "localhost", bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer " + token}, {"content-type", "application/json"}},
                   R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}");
    auto v = parse(r.body);
    if (!v.is_object() || !v.as_object().contains("result")) return bj::object{{"isError", true}, {"text", r.body}};
    auto res = v.as_object().at("result").as_object();
    res["text"] = res.at("content").as_array().at(0).at("text");
    return res;
  }
  void focus_grok(const std::string& tenants) {
    auto boss = sign_in("boss@example.com");
    auto r = api(boss, "write/scope", R"({"operator":"token:grok","tenants":)" + tenants + R"(,"confirm":true})");
    ASSERT_EQ(r.status, 200) << r.body;
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string token;
};

TEST_F(ScopeTest, EveryToolHasARule) {
  // Deny by default: a tool without a rule is refused for a focused operator, so every
  // registered tool must be classified.
  for (const auto& name : host->operator_tools().names()) {
    EXPECT_TRUE(host::Host::scope_rule(name).has_value()) << name << " has no scope rule (operator_scope.cpp)";
  }
}

TEST_F(ScopeTest, AllUntilAHumanNarrowsIt) {
  EXPECT_EQ(call("operator_scope").at("structuredContent").at("scope"), "all");
  EXPECT_FALSE(call("host_health").at("isError").as_bool());
  focus_grok(R"(["kc"])");
  auto me = call("operator_scope").at("structuredContent").as_object();
  EXPECT_EQ(me.at("scope").as_array().at(0), "kc");
  EXPECT_EQ(me.at("key"), "token:grok");
  // No tool changes a scope: an agent can't widen its own.
  for (const auto& name : host->operator_tools().names()) EXPECT_EQ(name.find("scope_set"), std::string::npos) << name;
}

TEST_F(ScopeTest, TenantToolsStayInside) {
  focus_grok(R"(["kc"])");
  auto in = call("content_put", R"({"tenant":"kc","path":"a.txt","text":"hi"})");
  EXPECT_FALSE(in.at("isError").as_bool()) << in.at("text");
  auto out = call("content_put", R"({"tenant":"other","path":"a.txt","text":"hi"})");
  EXPECT_TRUE(out.at("isError").as_bool());
  EXPECT_NE(std::string(out.at("text").as_string()).find("focused on kc"), std::string::npos) << out.at("text");
  EXPECT_FALSE(host->content().get("other", "a.txt"));
  // Leaving out an optional tenant means every tenant: refused. A required one is filled in,
  // since only kc is possible.
  EXPECT_TRUE(call("changes", R"({})").at("isError").as_bool());
  EXPECT_FALSE(call("content_list", R"({})").at("isError").as_bool());
  EXPECT_FALSE(call("changes", R"({"tenant":"kc"})").at("isError").as_bool());
  // Host-wide tools are refused, and not listed.
  EXPECT_TRUE(call("host_health").at("isError").as_bool());
  auto list = fetch(port, "localhost", bhttp::verb::post, "/goober-operations-mcp",
                    {{"authorization", "Bearer " + token}, {"content-type", "application/json"}},
                    R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
  EXPECT_EQ(list.body.find("\"host_health\""), std::string::npos);
  EXPECT_NE(list.body.find("\"content_put\""), std::string::npos);
}

TEST_F(ScopeTest, BoxesManifestsAndLists) {
  focus_grok(R"(["kc"])");
  EXPECT_FALSE(call("version_list", R"({"box":"kcbox"})").at("isError").as_bool());
  // A box on "*" serves every tenant, so it's out of reach.
  auto star = call("version_list", R"({"box":"everywhere"})");
  EXPECT_TRUE(star.at("isError").as_bool());
  EXPECT_NE(std::string(star.at("text").as_string()).find("every tenant"), std::string::npos) << star.at("text");
  auto upload = call("version_upload", R"J({"manifest":{"box":"sneaky","domains":["other.example.com"],
      "routes":{"http":{"/s":{"get":{"name":"hi","public":true}}}}},"wat":"(module (memory (export \"memory\") 1) (func (export \"goober_route_hi\") (result i32) (i32.const 0)))"})J");
  EXPECT_TRUE(upload.at("isError").as_bool());
  EXPECT_TRUE(host->boxes().versions("sneaky").empty());
  // Lists keep only the scope's rows.
  auto boxes = call("box_list");
  std::string text(boxes.at("text").as_string());
  EXPECT_NE(text.find("kcbox"), std::string::npos);
  EXPECT_EQ(text.find("everywhere"), std::string::npos);
  auto domains = std::string(call("domain_list").at("text").as_string());
  EXPECT_EQ(domains.find("other.example.com"), std::string::npos);
  EXPECT_NE(domains.find("kc.example.com"), std::string::npos);
  EXPECT_TRUE(call("route_check", R"({"domain":"other.example.com","path":"/","verb":"get"})").at("isError").as_bool());
}

TEST_F(ScopeTest, OnlyHumansInThePanelChangeIt) {
  auto boss = sign_in("boss@example.com");
  auto helper = sign_in("helper@example.com");
  auto watcher = sign_in("watcher@example.com");
  // An observer can't; an operator can't touch another's; a cross-origin post is refused.
  EXPECT_EQ(api(watcher, "write/scope", R"({"operator":"person:watcher@example.com","tenants":["kc"],"confirm":true})").status, 403);
  EXPECT_EQ(api(helper, "write/scope", R"({"operator":"token:grok","tenants":["kc"],"confirm":true})").status, 403);
  auto cross = fetch(port, kOps, bhttp::verb::post, "/api/write/scope",
                     {{"cookie", boss}, {"content-type", "application/json"}, {"origin", "https://evil.example"}},
                     R"({"operator":"token:grok","tenants":["kc"],"confirm":true})");
  EXPECT_EQ(cross.status, 403);
  EXPECT_EQ(api(boss, "write/scope", R"({"operator":"token:grok","tenants":["kc"]})").status, 400);  // no confirm
  // An operator narrows their own, and can't widen it back.
  EXPECT_EQ(api(helper, "write/scope", R"({"operator":"person:helper@example.com","tenants":["kc"],"confirm":true})").status, 200);
  EXPECT_EQ(api(helper, "write/scope", R"({"operator":"person:helper@example.com","tenants":"all","confirm":true})").status, 403);
  EXPECT_EQ(api(helper, "write/scope", R"({"operator":"person:helper@example.com","tenants":["kc","other"],"confirm":true})").status, 403);
  // Their panel follows: tenant pickers, and the tools.
  auto me = parse(api(helper, "me").body).as_object();
  EXPECT_EQ(me.at("focus").as_array().size(), 1u);
  EXPECT_EQ(me.at("tenants").as_array().size(), 1u);
  EXPECT_EQ(api(helper, "host_health").status, 403);
  EXPECT_EQ(api(helper, "content_list", R"({"tenant":"other"})").status, 403);
  EXPECT_EQ(api(helper, "content_list", R"({"tenant":"kc"})").status, 200);
  // The owner widens anyone, and the change log has each change.
  EXPECT_EQ(api(boss, "write/scope", R"({"operator":"person:helper@example.com","tenants":"all","confirm":true})").status, 200);
  EXPECT_EQ(api(helper, "host_health").status, 200);
  EXPECT_EQ(api(boss, "write/scope", R"({"operator":"token:grok","tenants":["nope"],"confirm":true})").status, 400);
  auto ops = parse(api(boss, "operators").body).as_object().at("result").as_object();
  EXPECT_TRUE(ops.at("may_set_any").as_bool());
  EXPECT_GE(ops.at("operators").as_array().size(), 4u);
  auto actions = parse(api(boss, "operator_actions").body).as_object().at("result");
  EXPECT_NE(json::dump(actions).find("operator_scope_set"), std::string::npos);
}

TEST_F(ScopeTest, AnAgentSignedInAsAFocusedPersonIsFocused) {
  EXPECT_EQ(host::Host::scope_key("oauth:helper@example.com:grok-client"), "person:helper@example.com");
  EXPECT_EQ(host::Host::scope_key("panel:helper@example.com"), "person:helper@example.com");
  host::Host::OperatorScope kc;
  kc.all = false;
  kc.tenants = {"kc"};
  std::string why;
  ASSERT_TRUE(host->set_operator_scope("person:helper@example.com", kc, "test", &why)) << why;
  auto policy = host->operator_policy("oauth:helper@example.com:grok-client", false);
  ASSERT_TRUE(policy.check);
  EXPECT_TRUE(policy.check("content_put", bj::object{{"tenant", "other"}}));
  EXPECT_FALSE(policy.check("content_put", bj::object{{"tenant", "kc"}}));
}

// Approvals per connected agent (035): off by default; on, a changing call waits for a person.
TEST_F(ScopeTest, ApprovalsHoldAnAgentsChangesUntilAPersonDecides) {
  // Off: the call runs.
  EXPECT_FALSE(call("content_put", R"({"tenant":"kc","path":"a.txt","text":"one"})").at("isError").as_bool());
  auto boss = sign_in("boss@example.com");
  auto helper = sign_in("helper@example.com");
  auto watcher = sign_in("watcher@example.com");
  // The agent is listed once it has called; only an owner turns approvals on for a token.
  auto listed = api(boss, "approvals");
  EXPECT_NE(listed.body.find("\"agent\":\"token:grok\""), std::string::npos) << listed.body;
  EXPECT_EQ(api(helper, "write/approvals", R"({"agent":"token:grok","on":true,"confirm":true})").status, 403);
  EXPECT_EQ(api(watcher, "write/approvals", R"({"agent":"token:grok","on":true,"confirm":true})").status, 403);
  // The panel only looks: an observer can read the unsubscribed list but not block an address.
  EXPECT_EQ(api(watcher, "mail_unsubscribed", R"({"tenant":"kc","email":"a@example.com","set":true})").status, 403);
  EXPECT_EQ(api(boss, "write/approvals", R"({"agent":"token:grok","on":true})").status, 400);  // unconfirmed
  ASSERT_EQ(api(boss, "write/approvals", R"({"agent":"token:grok","on":true,"confirm":true})").status, 200);
  // On: looking still runs; changing waits, and nothing changed.
  EXPECT_FALSE(call("content_list", R"({"tenant":"kc"})").at("isError").as_bool());
  // Whether a call looks depends on its arguments: a dry run and a checkout never wait;
  // mail_unsubscribed waits when it would block an address.
  auto not_held = [&](const char* tool, const char* args) {
    auto r = call(tool, args);
    EXPECT_EQ(std::string(r.at("text").as_string()).find("pending_approval"), std::string::npos) << tool << " " << args;
  };
  not_held("version_upload", R"j({"manifest":{"box":"x","domains":["*"]},"wat":"(module)","dry_run":true})j");
  not_held("source_checkout", R"({"project":"nope"})");
  not_held("mail_unsubscribed", R"({"tenant":"kc"})");
  EXPECT_NE(std::string(call("mail_unsubscribed", R"({"tenant":"kc","email":"a@example.com","set":true})").at("text").as_string())
                .find("pending_approval"),
            std::string::npos);
  // Staging builds only the agent's own draft, and a content dry run stores nothing: neither
  // waits. Deploying the draft waits, and what the person approves names every staged item.
  not_held("content_stage", R"({"tenant":"kc","files":[{"path":"staged/one.txt","text":"1"}]})");
  not_held("content_deploy", R"({"tenant":"kc","stage":true,"dry_run":true})");
  auto staged = std::string(call("content_deploy", R"({"tenant":"kc","stage":true})").at("text").as_string());
  EXPECT_NE(staged.find("pending_approval"), std::string::npos) << staged;
  EXPECT_NE(api(boss, "approvals").body.find("staged/one.txt"), std::string::npos);
  EXPECT_FALSE(base::read_file(root / "storage/content/kc/staged/one.txt"));
  auto held = call("content_put", R"({"tenant":"kc","path":"a.txt","text":"two"})");
  EXPECT_FALSE(held.at("isError").as_bool());
  auto text = std::string(held.at("text").as_string());
  std::smatch m;
  ASSERT_TRUE(std::regex_search(text, m, std::regex(R"re("pending_approval": ([0-9]+))re"))) << text;
  std::string id = m[1];
  EXPECT_EQ(*base::read_file(root / "storage/content/kc/a.txt"), "one");
  // A scope refusal is still a refusal, not a request.
  focus_grok(R"(["kc"])");
  EXPECT_TRUE(call("content_put", R"({"tenant":"other","path":"a.txt","text":"x"})").at("isError").as_bool());
  // The agent sees its request; a helper can't decide for a token; the owner approves, once.
  EXPECT_NE(std::string(call("approval_status", "{\"id\":" + id + "}").at("text").as_string()).find("\"pending\""),
            std::string::npos);
  auto pending = api(boss, "approvals");
  EXPECT_NE(pending.body.find("\"tool\":\"content_put\""), std::string::npos) << pending.body;
  EXPECT_EQ(api(helper, "write/decide", "{\"id\":" + id + ",\"decision\":\"approve\",\"confirm\":true}").status, 403);
  auto ok = api(boss, "write/decide", "{\"id\":" + id + ",\"decision\":\"approve\",\"confirm\":true}");
  ASSERT_EQ(ok.status, 200) << ok.body;
  EXPECT_NE(ok.body.find("\"state\":\"done\""), std::string::npos) << ok.body;
  EXPECT_EQ(*base::read_file(root / "storage/content/kc/a.txt"), "two");
  EXPECT_EQ(api(boss, "write/decide", "{\"id\":" + id + ",\"decision\":\"approve\",\"confirm\":true}").status, 404);
  EXPECT_NE(std::string(call("approval_status", "{\"id\":" + id + "}").at("text").as_string()).find("\"done\""), std::string::npos);
  // Denied: never runs.
  auto again = std::string(call("content_put", R"({"tenant":"kc","path":"a.txt","text":"three"})").at("text").as_string());
  ASSERT_TRUE(std::regex_search(again, m, std::regex(R"re("pending_approval": ([0-9]+))re"))) << again;
  ASSERT_EQ(api(boss, "write/decide", "{\"id\":" + std::string(m[1]) + ",\"decision\":\"deny\",\"confirm\":true}").status, 200);
  EXPECT_EQ(*base::read_file(root / "storage/content/kc/a.txt"), "two");
  // The audit log says who held, approved, and denied.
  auto audit = *base::read_file(root / "storage/logs/audit.log");
  EXPECT_NE(audit.find("\"outcome\":\"held\""), std::string::npos);
  EXPECT_NE(audit.find("approval.approved"), std::string::npos);
  EXPECT_NE(audit.find("approval.denied"), std::string::npos);
  EXPECT_NE(audit.find("\"approved_by\":\"panel:boss@example.com\""), std::string::npos);
  // Off again: calls run at once.
  ASSERT_EQ(api(boss, "write/approvals", R"({"agent":"token:grok","on":false,"confirm":true})").status, 200);
  EXPECT_FALSE(call("content_put", R"({"tenant":"kc","path":"a.txt","text":"four"})").at("isError").as_bool());
  EXPECT_EQ(*base::read_file(root / "storage/content/kc/a.txt"), "four");
}

// 035: once a request is decided or expired, only the copy shown is kept: no secret in the store.
TEST_F(ScopeTest, ExpiredAndRefusedRequestsKeepNoSecrets) {
  auto boss = sign_in("boss@example.com");
  call("content_list", R"({"tenant":"kc"})");  // the agent is seen
  ASSERT_EQ(api(boss, "write/approvals", R"({"agent":"token:grok","on":true,"confirm":true})").status, 200);
  call("key_set", R"({"scope":"tenant","tenant":"kc","name":"k1","value":"sekrit-value-1","hosts":["api.example.com"]})");
  call("connection_put", R"({"tenant":"kc","box":"b","service":"s","client_secret":"sekrit-value-2"})");
  auto db_text = [&] {
    auto dir = root / "storage/operations";
    return base::read_file(dir / "approvals.db").value_or("") + base::read_file(dir / "approvals.db-wal").value_or("");
  };
  ASSERT_NE(db_text().find("sekrit-value-1"), std::string::npos);  // held: the real arguments, to run them
  {
    sqlite::Db db(root / "storage/operations/approvals.db");
    db.exec("UPDATE requests SET at_ms = 0");  // a day and more ago
  }
  auto listed = api(boss, "approvals");  // reading the store expires them
  EXPECT_NE(listed.body.find("expired"), std::string::npos) << listed.body;
  {
    sqlite::Db db(root / "storage/operations/approvals.db");
    db.exec("PRAGMA wal_checkpoint(TRUNCATE)");  // old frames (the WAL) are SQLite's to reuse
  }
  EXPECT_EQ(db_text().find("sekrit-value-1"), std::string::npos);
  EXPECT_EQ(db_text().find("sekrit-value-2"), std::string::npos);
}

// From the operator-surfaces review: holes a focused agent could reach through.
TEST_F(ScopeTest, FocusedAgentsCantReachThroughKeysDataBoxesOrVersions) {
  focus_grok(R"(["kc"])");
  // A system key is every tenant's.
  EXPECT_TRUE(call("key_set", R"({"scope":"system","tenant":"kc","name":"openai","value":"x","hosts":["api.openai.com"]})")
                  .at("isError").as_bool());
  EXPECT_TRUE(call("key_delete", R"({"scope":"system","tenant":"kc","name":"openai"})").at("isError").as_bool());
  EXPECT_FALSE(call("key_set", R"({"scope":"tenant","tenant":"kc","name":"mine","value":"x","hosts":["api.example.com"]})")
                   .at("isError").as_bool());
  // A strategy pinned to another tenant's database.
  auto pinned = call("version_upload", R"J({"manifest":{"box":"sneaky","domains":["kc.example.com"],"data":{"sqlite3":{"d":)J"
                                       R"J({"schema":"s","database":"x","tenant":"other","pattern":"sql","tables":{"t":{}}}}}},"wat":"(module)"})J");
  EXPECT_TRUE(pinned.at("isError").as_bool());
  EXPECT_NE(std::string(pinned.at("text").as_string()).find("tenant other"), std::string::npos) << json::dump(pinned);
  // Replacing another tenant's box under its name.
  std::string err;
  ASSERT_TRUE(host->boxes().upload(
      R"({"box":"otherbox","domains":["other.example.com"]})", kModule,
      &err)) << err;
  auto replaced = call("version_upload", R"J({"manifest":{"box":"otherbox","domains":["kc.example.com"]},"wat":"(module)"})J");
  EXPECT_TRUE(replaced.at("isError").as_bool());
  EXPECT_NE(std::string(replaced.at("text").as_string()).find("already serves"), std::string::npos) << json::dump(replaced);
  // Promoting a stored version that serves another tenant, of a box that's theirs now.
  auto v1 = host->boxes().upload(R"({"box":"flip","domains":["other.example.com"]})",
                                 kModule, &err);
  ASSERT_TRUE(v1) << err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"flip","domains":["kc.example.com"]})",
                                   kModule, &err)) << err;
  auto promote = call("version_promote", R"({"box":"flip","version":")" + v1->version + R"("})");
  EXPECT_TRUE(promote.at("isError").as_bool()) << json::dump(promote);
}

TEST_F(ScopeTest, BatchedCallsAreAuditedOneByOneAndHeldSecretsAreHidden) {
  auto r = fetch(port, "localhost", bhttp::verb::post, "/goober-operations-mcp",
                 {{"authorization", "Bearer " + token}, {"content-type", "application/json"}},
                 R"([{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"content_put","arguments":{"tenant":"kc","path":"b1.txt","text":"1"}}},)"
                 R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"content_put","arguments":{"tenant":"kc","path":"b2.txt","text":"2"}}}])");
  ASSERT_EQ(r.status, 200) << r.body;
  auto audit = *base::read_file(root / "storage/logs/audit.log");
  EXPECT_NE(audit.find("\"path\":\"b1.txt\""), std::string::npos);
  EXPECT_NE(audit.find("\"path\":\"b2.txt\""), std::string::npos);
  // A held key_set never shows its value, to the person or the agent, and isn't kept once decided.
  auto boss = sign_in("boss@example.com");
  ASSERT_EQ(api(boss, "write/approvals", R"({"agent":"token:grok","on":true,"confirm":true})").status, 200);
  auto held = std::string(call("key_set", R"({"scope":"tenant","tenant":"kc","name":"k","value":"sup3r-secret","hosts":["a.example"]})")
                              .at("text").as_string());
  EXPECT_EQ(api(boss, "approvals").body.find("sup3r-secret"), std::string::npos);
  std::smatch m;
  ASSERT_TRUE(std::regex_search(held, m, std::regex(R"re("pending_approval": ([0-9]+))re"))) << held;
  EXPECT_EQ(std::string(call("approval_status", "{\"id\":" + std::string(m[1]) + "}").at("text").as_string()).find("sup3r-secret"),
            std::string::npos);
  // The token is revoked while its request waits: approving denies it instead.
  ASSERT_EQ(host::revoke_operator_tokens(root / "storage", "grok"), 1);
  auto late = api(boss, "write/decide", "{\"id\":" + std::string(m[1]) + ",\"decision\":\"approve\",\"confirm\":true}");
  EXPECT_EQ(late.status, 409) << late.body;
  EXPECT_FALSE(host->keys().resolve("keys/tenant/{!tenant}/k", "kc"));
  auto db = *base::read_file(root / "storage/operations/approvals.db");
  EXPECT_EQ(db.find("sup3r-secret"), std::string::npos);
}

// A commit is checked as the project will be after it, even a project's first.
TEST_F(ScopeTest, ACommitCantWriteAnotherTenantsBoxIntoAProject) {
  focus_grok(R"(["kc"])");
  ASSERT_FALSE(call("source_create", R"({"project":"mine","tenant":"kc"})").at("isError").as_bool());
  auto sneaky = call("source_commit", R"J({"project":"mine","base_rev":0,"message":"x","changes":[
      {"path":"goober.project.json","text":"{\"boxes\":[\".\"]}"},
      {"path":"manifest.json","text":"{\"box\":\"theirs\",\"domains\":[\"other.example.com\"]}"}]})J");
  EXPECT_TRUE(sneaky.at("isError").as_bool()) << json::dump(sneaky);
  EXPECT_NE(std::string(sneaky.at("text").as_string()).find("other"), std::string::npos);
  auto fine = call("source_commit", R"J({"project":"mine","base_rev":0,"message":"x","changes":[
      {"path":"goober.project.json","text":"{\"boxes\":[\".\"]}"},
      {"path":"manifest.json","text":"{\"box\":\"ours\",\"domains\":[\"kc.example.com\"]}"}]})J");
  EXPECT_FALSE(fine.at("isError").as_bool()) << json::dump(fine);
}
