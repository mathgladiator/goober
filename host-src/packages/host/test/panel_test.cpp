// The operator panel (design/machine/028.Panel.md): its own origin, host-only sign-in, the
// read tools through /api/, the change log and rollups behind it, and host:observer over MCP.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

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

bj::object parse(const std::string& s) {
  std::string err;
  auto v = json::parse(s, &err);
  return v && v->is_object() ? v->as_object() : bj::object{};
}

const std::vector<std::pair<std::string, std::string>> kForm = {{"content-type", "application/x-www-form-urlencoded"}};

}  // namespace

class PanelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "panel";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"localhost": {"tenant": "playground"}},
        "operations": {"token": "0123456789abcdef0123", "domain": "ops.example.com", "listen": "127.0.0.1:0"},
        "identity": {"operators": {"boss@example.com": ["host:owner"], "watcher@example.com": ["host:observer"],
                                   "helper@example.com": ["host:support"]}},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  std::optional<std::string> newest_code(const std::string& email) {
    host->drain_mail();
    auto files = base::list_files(root / "storage/mail/outbox");
    std::sort(files.begin(), files.end());
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
      auto text = *base::read_file(root / "storage/mail/outbox" / *it);
      std::smatch m;
      if (text.find("To: " + email) != std::string::npos && std::regex_search(text, m, std::regex("is ([0-9]{6})"))) {
        return m[1];
      }
    }
    return std::nullopt;
  }
  // Signs in on the panel's origin with the plain form: no "as=host" is sent, and none is needed.
  std::string sign_in(const std::string& email) {
    fetch(port, kOps, bhttp::verb::post, "/identity/email/start", kForm, "email=" + email);
    auto code = newest_code(email);
    if (!code) return "";
    auto v = fetch(port, kOps, bhttp::verb::post, "/identity/email/verify", kForm, "email=" + email + "&code=" + *code);
    EXPECT_EQ(v.status, 303) << v.body;
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }
  Reply api(const std::string& cookie, const std::string& tool, const std::string& args = "{}") {
    return fetch(port, kOps, bhttp::verb::post, "/api/" + tool,
                 {{"cookie", cookie}, {"content-type", "application/json"}, {"origin", "http://" + kOps}}, args);
  }
  Reply mcp(const std::string& bearer, const std::string& body) {
    return fetch(port, "localhost", bhttp::verb::post, "/goober-operations-mcp",
                 {{"authorization", "Bearer " + bearer}, {"content-type", "application/json"}}, body);
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(PanelTest, SignedOutTheOriginSendsYouToHostSignIn) {
  auto page = fetch(port, kOps, bhttp::verb::get, "/");
  EXPECT_EQ(page.status, 303);
  EXPECT_EQ(page.headers["location"], "/identity/sign-in/as-host?return=%2F");
  // On this origin even the site's sign-in is the host's.
  auto form = fetch(port, kOps, bhttp::verb::get, "/identity/sign-in");
  EXPECT_EQ(form.status, 200);
  EXPECT_NE(form.body.find("Host sign-in"), std::string::npos);
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::get, "/api/me").status, 401);
  // The script and styles hold no data, and carry the CSP.
  auto js = fetch(port, kOps, bhttp::verb::get, "/panel.js");
  EXPECT_EQ(js.status, 200);
  EXPECT_NE(js.headers["content-security-policy"].find("script-src 'self'"), std::string::npos);
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::get, "/panel.js", {{"if-none-match", js.headers["etag"]}}).status, 304);
}

TEST_F(PanelTest, TheOriginServesThePanelAloneAndNoTenantServesIt) {
  // Nothing of the site's on the panel's origin, but health and the operator MCP endpoint.
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::get, "/about").status, 404);
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::post, "/mcp", {{"content-type", "application/json"}}, "{}").status, 404);
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::get, "/oauth/authorize").status, 404);
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::get, "/goober/health").status, 200);
  // And no panel on the tenant's origin.
  EXPECT_EQ(fetch(port, "localhost", bhttp::verb::get, "/panel.js").status, 404);
  EXPECT_EQ(fetch(port, "localhost", bhttp::verb::get, "/api/me").status, 404);
}

TEST_F(PanelTest, AnOperatorLooksThroughTheReadTools) {
  std::string cookie = sign_in("boss@example.com");
  ASSERT_TRUE(cookie.starts_with("goober-session=h.")) << cookie;
  auto page = fetch(port, kOps, bhttp::verb::get, "/", {{"cookie", cookie}});
  EXPECT_EQ(page.status, 200);
  EXPECT_NE(page.body.find("/panel.js"), std::string::npos);
  EXPECT_NE(page.headers["content-security-policy"].find("frame-ancestors 'none'"), std::string::npos);
  auto me = parse(fetch(port, kOps, bhttp::verb::get, "/api/me", {{"cookie", cookie}}).body);
  EXPECT_EQ(me["email"], "boss@example.com");
  EXPECT_EQ(me["tenants"].as_array().at(0), "playground");

  auto health = api(cookie, "host_health");
  ASSERT_EQ(health.status, 200) << health.body;
  EXPECT_TRUE(parse(health.body)["result"].as_object().contains("build"));
  // Look-only, even for the owner.
  auto refused = api(cookie, "content_put", R"({"tenant":"playground","path":"x.html","text":"x"})");
  EXPECT_EQ(refused.status, 403);
  EXPECT_FALSE(host->content().get("playground", "x.html"));
  // JSON from this origin only.
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::post, "/api/host_health", {{"cookie", cookie}, {"origin", "http://" + kOps}}, "{}").status,
            415);
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::post, "/api/host_health",
                  {{"cookie", cookie}, {"content-type", "application/json"}, {"origin", "https://evil.example"}}, "{}")
                .status,
            403);
}

TEST_F(PanelTest, ARoleOutsideThePanelsIsRefusedAndSitePeopleGetNoCode) {
  std::string helper = sign_in("helper@example.com");
  ASSERT_FALSE(helper.empty());
  EXPECT_EQ(fetch(port, kOps, bhttp::verb::get, "/", {{"cookie", helper}}).status, 403);
  EXPECT_EQ(api(helper, "host_health").status, 403);
  // Someone who isn't an operator gets no code here, so no account is made on this origin.
  EXPECT_EQ(sign_in("visitor@example.com"), "");
}

TEST_F(PanelTest, TheChangeLogNamesTheOperatorAndTheRollupsCountThePanel) {
  std::string token = "0123456789abcdef0123";
  auto put = mcp(token, R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"content_put",
      "arguments":{"tenant":"playground","path":"hello.md","text":"# Hi\n"}}})");
  ASSERT_EQ(put.status, 200) << put.body;
  mcp(token, R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"content_put",
      "arguments":{"tenant":"playground","path":"hello.md","text":"# Hello\n"}}})");
  std::string cookie = sign_in("boss@example.com");
  auto changes = parse(api(cookie, "changes", R"({"kind":"content"})").body)["result"].as_object()["changes"].as_array();
  ASSERT_GE(changes.size(), 2u);
  const auto& newest = changes[0].as_object();
  EXPECT_EQ(newest.at("actor"), "config");
  EXPECT_EQ(newest.at("target"), "hello.md");
  EXPECT_EQ(newest.at("action"), "write");
  auto one = parse(api(cookie, "change_get", "{\"id\":" + std::to_string(newest.at("id").as_int64()) + "}").body)["result"];
  EXPECT_EQ(one.at("before"), "# Hi\n");
  EXPECT_EQ(one.at("after"), "# Hello\n");
  auto actions = parse(api(cookie, "operator_actions", R"({"changed_only":true})").body)["result"].as_object()["actions"].as_array();
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions[0].as_object().at("tool"), "content_put");
  EXPECT_EQ(actions[0].as_object().at("args").as_object().at("path"), "hello.md");
  EXPECT_EQ(actions[0].as_object().at("changes").as_array().size(), 1u);

  auto traffic = parse(api(cookie, "traffic", R"({"window":"1h"})").body)["result"].as_object();
  bool saw_operations = false;
  for (const auto& r : traffic["routes"].as_array()) saw_operations |= r.as_object().at("route") == "operations";
  EXPECT_TRUE(saw_operations);
  EXPECT_EQ(traffic["series"].as_object()["points"].as_array().size(), 60u);
  auto graph = parse(api(cookie, "box_graph").body)["result"].as_object();
  bool tenant_node = false;
  for (const auto& n : graph["nodes"].as_array()) tenant_node |= n.as_object().at("id") == "tenant:playground";
  EXPECT_TRUE(tenant_node);
}

TEST_F(PanelTest, AnObserverOnlyLooksOverMcpToo) {
  // A host-realm agent client, and a token for the observer (who must exist: sign in once).
  ASSERT_FALSE(sign_in("watcher@example.com").empty());
  std::string why;
  identity::Client client{"watch-agent", "Watcher's agent", {"https://agent.example/cb"}, false, "operator", 0};
  ASSERT_TRUE(host->identity().put_client(identity::kHostRealm, client, base::now_unix_ms(), &why)) << why;
  auto token = host->identity().mint_agent_token(identity::kHostRealm, "watch-agent", "watcher@example.com",
                                                 3600 * 1000, base::now_unix_ms(), &why);
  ASSERT_TRUE(token) << why;
  std::string err;
  auto list = parse(mcp(*token, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").body);
  std::set<std::string> names;
  for (const auto& t : list["result"].as_object()["tools"].as_array()) names.insert(std::string(t.at("name").as_string()));
  EXPECT_TRUE(names.count("host_health"));
  EXPECT_TRUE(names.count("changes"));
  EXPECT_FALSE(names.count("content_put"));
  EXPECT_FALSE(names.count("db_exec"));
  auto refused = parse(mcp(*token, R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"content_put",
      "arguments":{"tenant":"playground","path":"x.md","text":"x"}}})").body);
  EXPECT_TRUE(refused.contains("error"));
  EXPECT_FALSE(host->content().get("playground", "x.md"));
  // The owner's token still lists everything.
  auto all = parse(mcp("0123456789abcdef0123", R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})").body);
  EXPECT_GT(all["result"].as_object()["tools"].as_array().size(), names.size());
}

TEST_F(PanelTest, ThePrivateListenerIsAllPanel) {
  uint16_t panel = host->panel_port();
  ASSERT_NE(panel, 0);
  // Whatever the Host header says, it's the panel.
  EXPECT_EQ(fetch(panel, "localhost", bhttp::verb::get, "/").status, 303);
  EXPECT_EQ(fetch(panel, "localhost", bhttp::verb::get, "/panel.css").status, 200);
  EXPECT_EQ(fetch(panel, "localhost", bhttp::verb::get, "/about").status, 404);
}
