// Operator scope (design/machine/030.OperatorScope.md): an operator focused on some tenants
// reaches only them, over MCP and in the panel, and only a human changes it, in the panel.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

#include "goober/base/encoding.h"
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
  auto ops = parse(api(boss, "operators").body).as_object();
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
