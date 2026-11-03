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
#include "goober/source/source.h"

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
  EXPECT_EQ(traffic["series"].as_object()["points"].as_array().size(), 12u);
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

// The Source view's reads (020 § 9): projects, a revision's files and diff, a file, the kept
// jobs with their logs, and a box's versions back to their source. Building stays MCP's.
TEST_F(PanelTest, TheSourceViewReadsProjectsJobsAndBoxHistory) {
  auto& src = host->source();
  std::string err;
  ASSERT_TRUE(src.create("greeter", "says hello", 1, &err)) << err;
  ASSERT_TRUE(src.commit("greeter", 0, {{"src/lib.rs", "fn hi() {}\n"}}, "Start", "config", 10, &err)) << err;
  ASSERT_TRUE(src.commit("greeter", 1, {{"src/lib.rs", "fn hi() { howdy(); }\n"}}, "Say howdy", "config", 20, &err)) << err;
  src.save_job(source::JobRecord{3, "greeter", "deploy", 2, "failed", "config", 25, 26, 30, "✗ box greeter: no\n", "{}"});
  src.record_deploy(source::Deploy{3, "greeter", 2, 30, "config", {{"greeter", "v-abc"}}, ""});

  std::string cookie = sign_in("watcher@example.com");  // an observer looks too
  ASSERT_FALSE(cookie.empty());
  auto list = parse(api(cookie, "source_list").body)["result"];
  EXPECT_EQ(list.at(0).at("project"), "greeter");
  EXPECT_EQ(list.at(0).at("mirror").at("pending"), 3);  // backups are off here: two patches, one tree
  auto tree = parse(api(cookie, "source_tree", R"({"project":"greeter","rev":2})").body)["result"];
  EXPECT_EQ(tree.at("message"), "Say howdy");
  EXPECT_EQ(tree.at("changed").at(0), "src/lib.rs");
  auto diff = parse(api(cookie, "source_diff", R"({"project":"greeter","from":1,"to":2,"verbose":true})").body)["result"];
  EXPECT_NE(std::string(diff.at("diff").as_string()).find("+fn hi() { howdy(); }"), std::string::npos);
  auto file = parse(api(cookie, "source_read", R"({"project":"greeter","rev":1,"paths":["src/lib.rs"]})").body)["result"];
  EXPECT_EQ(file.at(0).at("text"), "fn hi() {}\n");
  auto jobs = parse(api(cookie, "source_job", R"({"project":"greeter"})").body)["result"];
  EXPECT_EQ(jobs.at(0).at("state"), "failed");
  auto job = parse(api(cookie, "source_job", R"({"job":3,"verbose":true})").body)["result"];
  EXPECT_EQ(job.at("log"), "✗ box greeter: no\n");
  auto history = parse(api(cookie, "source_box_history", R"({"box":"greeter"})").body)["result"];
  EXPECT_EQ(history.at("deploys").at(0).at("version"), "v-abc");
  EXPECT_EQ(history.at("deploys").at(0).at("message"), "Say howdy");
  EXPECT_EQ(api(cookie, "source_deploy", R"({"project":"greeter"})").status, 403);
  EXPECT_NE(fetch(port, kOps, bhttp::verb::get, "/panel.js", {{"cookie", cookie}}).body.find("async source()"), std::string::npos);
}

// The data flow down to tables (008 § 4): a box's graph shows each table its strategies grant,
// with the operations, dashed until a statement uses them; the Grants card reads grant_usage.
TEST_F(PanelTest, TheGraphShowsEachTableAndItsGrant) {
  std::string token = "0123456789abcdef0123";
  auto put = mcp(token, R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"schema_put",
      "arguments":{"schema":{"schema":"shop1","tables":{"orders":{"columns":{"paid":{"type":"boolean","default":false}}},
        "notes":{"columns":{"text":{"type":"string"}}}}}}}})");
  ASSERT_EQ(put.status, 200) << put.body;
  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"shop","domains":["*"],
      "data":{"sqlite3":{"db":{"schema":"shop1","database":"shop","pattern":"sql",
        "tables":{"orders":{"read":["paid"],"update":["paid"]},"notes":{"insert":true}}}}},
      "routes":{"http":{"/x":{"get":{"name":"x","public":true}}}}})",
                                   R"((module (memory (export "memory") 1) (func (export "goober_route_x") (result i32) (i32.const 0))))",
                                   &err))
      << err;
  std::string cookie = sign_in("watcher@example.com");
  ASSERT_FALSE(cookie.empty());
  auto graph = parse(api(cookie, "box_graph", R"({"box":"shop"})").body)["result"].as_object();
  std::map<std::string, bj::object> nodes, edges;
  for (const auto& n : graph["nodes"].as_array()) nodes[std::string(n.at("id").as_string())] = n.as_object();
  for (const auto& e : graph["edges"].as_array()) edges[std::string(e.at("to").as_string())] = e.as_object();
  ASSERT_TRUE(nodes.count("table:shop.orders")) << json::dump(graph);
  EXPECT_EQ(nodes["table:shop.orders"].at("column"), 3);
  EXPECT_EQ(nodes["table:shop.orders"].at("database"), "shop");
  EXPECT_EQ(edges["table:shop.orders"].at("from"), "db:shop");
  EXPECT_EQ(edges["table:shop.orders"].at("ops"), "ru");
  EXPECT_EQ(edges["table:shop.notes"].at("ops"), "i");
  EXPECT_EQ(edges["table:shop.notes"].at("unused"), true);
  EXPECT_EQ(edges["table:shop.orders"].at("grants").at(0).at("read").at(0), "paid");
  auto usage = parse(api(cookie, "grant_usage", R"({"box":"shop"})").body)["result"];
  EXPECT_EQ(usage.at(0).at("strategies").at(0).at("tables").at(0).at("findings").at(0).at("kind"), "unused_table");
  EXPECT_NE(fetch(port, kOps, bhttp::verb::get, "/panel.js", {{"cookie", cookie}}).body.find("function grantsCard"),
            std::string::npos);
}

// External services (028 § 4.4) and services in the data flow (§ 6): what went out, by service,
// from the egress and mail logs, and the graph's service nodes, the host's calls, and the mailer.
TEST_F(PanelTest, ServicesAreReportedAndDrawnWithTheMailer) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"notify","domains":["*"],
      "external":{"mail":{"mail":{}},
                  "pay":{"base":"https://pay.example","methods":{"charge":{"verb":"post","path":"/v1/charge"}}}},
      "routes":{"http":{"/x":{"get":{"name":"x","public":true}}}}})",
                                   R"((module (memory (export "memory") 1) (func (export "goober_route_x") (result i32) (i32.const 0))))",
                                   &err))
      << err;
  std::string now = base::format_utc_ms(base::now_unix_ms());
  auto egress = [&](const std::string& rest) {
    base::append_line(root / "storage/logs/egress.log", R"({"at":")" + now + R"(",)" + rest);
  };
  egress(R"("tenant":"playground","box":"notify","service":"pay","method":"charge","verb":"POST","status":200,"us":1500})");
  egress(R"("tenant":"playground","box":"notify","service":"pay","method":"charge","verb":"POST","status":502,"us":900,"url":"https://pay.example/v1/charge","request_id":"req-1"})");
  egress(R"("tenant":"playground","box":"notify","service":"mail","method":"send","verb":"MAIL","status":200,"us":300,"outbox_id":5})");
  egress(R"("tenant":"playground","box":"","service":"push","method":"send","verb":"POST","status":201,"us":700})");
  base::append_line(root / "storage/logs/mail.log",
                    R"({"at":")" + now + R"(","tenant":"playground","via":"dev","status":"sent","us":10})");

  std::string cookie = sign_in("watcher@example.com");
  ASSERT_FALSE(cookie.empty());
  auto r = parse(api(cookie, "service_report", R"({"window":"1h"})").body)["result"].as_object();
  std::map<std::string, bj::object> svcs;
  for (const auto& s : r["services"].as_array()) svcs[std::string(s.at("service").as_string())] = s.as_object();
  ASSERT_TRUE(svcs.count("pay")) << json::dump(r);
  EXPECT_EQ(svcs["pay"].at("kind"), "external");
  EXPECT_EQ(svcs["pay"].at("calls"), 2);
  EXPECT_EQ(svcs["pay"].at("n5"), 1);
  EXPECT_EQ(svcs["mail"].at("kind"), "mail");
  EXPECT_EQ(svcs["mail"].at("outbox"), 1);
  EXPECT_EQ(svcs["push"].at("kind"), "push");
  ASSERT_TRUE(svcs.count("mailer"));
  EXPECT_EQ(svcs["mailer"].at("kind"), "mailer");
  EXPECT_GE(svcs["mailer"].at("calls").as_int64(), 2);  // the box's mail, and the watcher's sign-in code
  EXPECT_EQ(r["latest"].at(0).at("request_id"), "req-1");
  EXPECT_EQ(r["mail"].at("mailer"), "dev");
  EXPECT_TRUE(r["outbox"].as_object().contains("points"));
  EXPECT_TRUE(r.contains("backups"));
  // One service, for a tenant: no backups (they are the host's).
  auto pay = parse(api(cookie, "service_report", R"({"window":"1h","tenant":"playground","service":"pay"})").body)["result"];
  EXPECT_EQ(pay.at("services").as_array().size(), 1u);
  EXPECT_FALSE(pay.as_object().contains("backups"));

  auto graph = parse(api(cookie, "box_graph", R"({"window":"1h"})").body)["result"].as_object();
  std::map<std::string, bj::object> nodes;
  std::set<std::pair<std::string, std::string>> edges;
  for (const auto& n : graph["nodes"].as_array()) nodes[std::string(n.at("id").as_string())] = n.as_object();
  for (const auto& e : graph["edges"].as_array()) {
    edges.emplace(std::string(e.at("from").as_string()), std::string(e.at("to").as_string()));
  }
  ASSERT_TRUE(nodes.count("svc:pay")) << json::dump(graph);
  EXPECT_EQ(nodes["svc:pay"].at("calls"), 2);
  EXPECT_EQ(nodes["svc:pay"].at("failed"), 1);
  EXPECT_EQ(nodes["svc:mail"].at("mail"), true);
  EXPECT_TRUE(edges.count({"box:notify", "svc:pay"}));
  EXPECT_TRUE(edges.count({"svc:mail", "svc:mailer"})) << json::dump(graph["edges"]);
  EXPECT_TRUE(edges.count({"host:playground", "svc:push"})) << json::dump(graph["edges"]);
  EXPECT_EQ(nodes["svc:mailer"].at("column"), 3);
  // One box's graph has its mailer too.
  auto one = parse(api(cookie, "box_graph", R"({"window":"1h","box":"notify"})").body)["result"].as_object();
  EXPECT_NE(json::dump(one).find("svc:mailer"), std::string::npos);
  auto js = fetch(port, kOps, bhttp::verb::get, "/panel.js", {{"cookie", cookie}}).body;
  EXPECT_NE(js.find("async services(params)"), std::string::npos);
  EXPECT_NE(js.find("function keyGraphs"), std::string::npos);
  EXPECT_NE(fetch(port, kOps, bhttp::verb::get, "/", {{"cookie", cookie}}).body.find("#/services"), std::string::npos);
}

// The Logs and uploads view (028 § 4.7): an observer reads log_velocity and backup_status.
TEST_F(PanelTest, AnObserverSeesLogVolumeAndUploads) {
  std::string cookie = sign_in("watcher@example.com");
  auto v = parse(api(cookie, "log_velocity", R"({"window":"1h"})").body);
  ASSERT_TRUE(v.contains("result")) << json::dump(v);
  EXPECT_TRUE(v.at("result").as_object().contains("series"));
  EXPECT_TRUE(v.at("result").as_object().contains("disk"));
  auto js = fetch(port, kOps, bhttp::verb::get, "/panel.js", {{"cookie", cookie}}).body;
  EXPECT_NE(js.find("async volume(params)"), std::string::npos);
  EXPECT_NE(fetch(port, kOps, bhttp::verb::get, "/", {{"cookie", cookie}}).body.find("#/volume"), std::string::npos);
}

// The Studies view (048 § 8): the panel lists and reads heat studies, and never starts one.
TEST_F(PanelTest, TheStudiesViewReadsButNeverStarts) {
  std::string cookie = sign_in("watcher@example.com");
  auto list = parse(api(cookie, "heat_study_list").body);
  ASSERT_TRUE(list.contains("result")) << json::dump(list);
  EXPECT_TRUE(list.at("result").is_array());
  EXPECT_EQ(api(cookie, "heat_study_start", R"({"domain":"localhost","why":"x"})").status, 403);
  EXPECT_EQ(api(cookie, "heat_study_stop", R"({"id":1})").status, 403);
  auto js = fetch(port, kOps, bhttp::verb::get, "/panel.js", {{"cookie", cookie}}).body;
  EXPECT_NE(js.find("async studies(params)"), std::string::npos);
  EXPECT_NE(fetch(port, kOps, bhttp::verb::get, "/", {{"cookie", cookie}}).body.find("#/studies"), std::string::npos);
}

TEST_F(PanelTest, ThePrivateListenerIsAllPanel) {
  uint16_t panel = host->panel_port();
  ASSERT_NE(panel, 0);
  // Whatever the Host header says, it's the panel.
  EXPECT_EQ(fetch(panel, "localhost", bhttp::verb::get, "/").status, 303);
  EXPECT_EQ(fetch(panel, "localhost", bhttp::verb::get, "/panel.css").status, 200);
  EXPECT_EQ(fetch(panel, "localhost", bhttp::verb::get, "/about").status, 404);
}

TEST_F(PanelTest, ReadySaysWhatEverySubsystemIs) {
  auto r = fetch(port, "localhost", bhttp::verb::get, "/goober/ready");
  ASSERT_EQ(r.status, 200) << r.body;
  auto ready = parse(r.body);
  EXPECT_EQ(ready["ok"], true);
  for (const char* k : {"storage", "disk", "identity", "boxes", "mcp", "change_log", "traffic", "queue"}) {
    EXPECT_TRUE(ready["checks"].as_object().contains(k)) << k;
  }
  // The disk the storage is on: free and total, so a filling disk is seen before writes fail.
  const auto& disk = ready["checks"].as_object()["disk"].as_object();
  EXPECT_GT(disk.at("total_bytes").as_int64(), 0);
  EXPECT_GE(disk.at("total_bytes").as_int64(), disk.at("free_bytes").as_int64());
  EXPECT_EQ(ready["checks"].as_object()["change_log"].as_object()["on"], true);
  EXPECT_FALSE(ready["build"].as_string().empty());
  EXPECT_TRUE(ready["deploy"].is_null());
  EXPECT_EQ(fetch(port, "localhost", bhttp::verb::get, "/goober/deploy").status, 404);
  // What the server's own check after a deploy wrote: served whole, and summed up in ready.
  std::string err;
  fs::create_directories(root / "storage/operations");
  ASSERT_TRUE(base::write_file_durable(root / "storage/operations/deploy-check.json",
      R"({"at":"2026-10-29T15:10:00Z","build":"4f575dadb5d7","ok":true,"failed":[],"checks":[{"check":"GET /","ok":true,"detail":""}]})"));
  auto deploy = fetch(port, "localhost", bhttp::verb::get, "/goober/deploy");
  EXPECT_EQ(deploy.status, 200);
  EXPECT_EQ(parse(deploy.body)["checks"].as_array().size(), 1u);
  auto brief = parse(fetch(port, "localhost", bhttp::verb::get, "/goober/ready").body)["deploy"].as_object();
  EXPECT_EQ(brief["build"], "4f575dadb5d7");
  EXPECT_FALSE(brief.contains("checks"));
}

// Health a monitor graphs (028 § 4.6): ratios, latencies, ages, and gauges, never volumes; and
// the same as Prometheus text.
TEST_F(PanelTest, ReadyCarriesHealthToGraph) {
  std::string err;
  ASSERT_TRUE(host->content().put("playground", "x.md", "# x", &err)) << err;
  for (int i = 0; i < 3; ++i) EXPECT_EQ(fetch(port, "localhost", bhttp::verb::get, "/x").status, 200);
  EXPECT_EQ(fetch(port, "localhost", bhttp::verb::get, "/no-such-page").status, 404);
  auto ready = parse(fetch(port, "localhost", bhttp::verb::get, "/goober/ready").body);
  const auto& h = ready["health"].as_object();
  EXPECT_EQ(h.at("window_s"), 300);
  EXPECT_GE(h.at("uptime_s").as_int64(), 0);
  const auto& req = h.at("requests").as_object();
  // One of four (the monitor's own polls aren't counted).
  EXPECT_DOUBLE_EQ(req.at("ratio_4xx").as_double(), 0.25);
  EXPECT_DOUBLE_EQ(req.at("ratio_5xx").as_double(), 0.0);
  EXPECT_GT(req.at("p99_ms").as_double(), 0.0);
  // No counts anywhere: they'd tell anyone the site's traffic.
  for (const char* k : {"n", "count", "requests", "total"}) EXPECT_FALSE(req.contains(k)) << k;
  for (const char* k : {"boxes", "mail", "services", "outbox", "workers", "disk", "backups", "logs"}) {
    EXPECT_TRUE(h.contains(k)) << k;
  }
  EXPECT_EQ(h.at("outbox").at("depth"), 0);
  EXPECT_TRUE(h.at("outbox").at("oldest_age_s").is_null());
  double free_ratio = h.at("disk").at("free_ratio").as_double();
  EXPECT_GT(free_ratio, 0.0);
  EXPECT_LE(free_ratio, 1.0);
  EXPECT_EQ(h.at("backups").at("enabled"), false);
  EXPECT_EQ(h.at("logs").at("write_errors"), 0);

  // Prometheus text, by query or by a scraper's Accept; 200 even when not ready.
  auto prom = fetch(port, "localhost", bhttp::verb::get, "/goober/ready?format=prometheus");
  ASSERT_EQ(prom.status, 200);
  EXPECT_NE(prom.headers["content-type"].find("text/plain"), std::string::npos);
  for (const char* want : {"goober_ready 1\n", "# TYPE goober_requests_5xx_ratio gauge\n", "goober_requests_4xx_ratio 0.25\n",
                           "goober_check_ok{check=\"disk\"} 1\n", "goober_outbox_depth 0\n",
                           "# TYPE goober_log_write_errors_total counter\n", "goober_disk_free_bytes "}) {
    EXPECT_NE(prom.body.find(want), std::string::npos) << want << "\n" << prom.body;
  }
  EXPECT_EQ(prom.body.find("goober_outbox_oldest_age_seconds"), std::string::npos) << "unknown is left out, not 0";
  EXPECT_EQ(prom.body.find("# TYPE goober_check_ok gauge"), prom.body.rfind("# TYPE goober_check_ok gauge"));
  auto scraped = fetch(port, "localhost", bhttp::verb::get, "/goober/ready", {{"accept", "text/plain;version=0.0.4"}});
  EXPECT_NE(scraped.body.find("goober_ready 1"), std::string::npos);
  auto down = host::Host::prometheus_text(bj::object{{"ok", false}, {"checks", bj::object{{"disk", bj::object{{"ok", false}}}}}});
  EXPECT_NE(down.find("goober_ready 0\n"), std::string::npos);
  EXPECT_NE(down.find("goober_check_ok{check=\"disk\"} 0\n"), std::string::npos);
}

// The pulse counts box calls, mail, and calls out as they're logged.
TEST_F(PanelTest, ThePulseCountsMailAndCallsOut) {
  int64_t now = base::now_unix_ms();
  host->log_line("mail", bj::object{{"to", "a@example.com"}, {"status", "sent"}});
  host->log_line("mail", bj::object{{"to", "b@example.com"}, {"status", "failed"}, {"error", "refused"}});
  host->log_line("mail", bj::object{{"to", "c@example.com"}, {"status", "failed"}, {"test", true}});
  host->log_line("egress", bj::object{{"service", "api"}, {"status", 200}});
  host->log_line("egress", bj::object{{"service", "api"}, {"status", 0}, {"error", "timed out"}});
  host->log_line("egress", bj::object{{"service", "api"}, {"status", 503}});
  host->log_line("egress", bj::object{{"service", "api"}, {"status", 201}});
  auto s = host->pulse().snapshot(now);
  using P = operations::Pulse;
  EXPECT_EQ(s.n[P::kMailSent], 1);
  EXPECT_EQ(s.n[P::kMailFailed], 1);
  EXPECT_EQ(s.n[P::kServiceCalls], 4);
  EXPECT_EQ(s.n[P::kServiceFailed], 2);
  auto h = host->health(now + 5000);
  EXPECT_DOUBLE_EQ(h.at("mail").at("failure_ratio").as_double(), 0.5);
  EXPECT_DOUBLE_EQ(h.at("services").at("failure_ratio").as_double(), 0.5);
}

// The panel's stores are never a reason not to start (the 2026-10-29 outage's lesson).
TEST(PanelStartup, AHostWhoseChangeLogCantOpenStillServes) {
  auto root = fs::path(GOOBER_TEST_TMP) / "panel_degraded";
  fs::remove_all(root);
  // A directory where each database file would go: SQLite can't open either.
  fs::create_directories(root / "storage/operations/changes.db");
  fs::create_directories(root / "storage/operations/traffic.db");
  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1}})", root, &err);
  ASSERT_TRUE(cfg) << err;
  auto host = std::make_unique<host::Host>(config::local_mode(cfg));
  uint16_t port = host->start();
  bool noted = false;
  for (const auto& n : host->startup_notes()) noted |= n.starts_with("change log off");
  EXPECT_TRUE(noted);
  auto r = fetch(port, "localhost", bhttp::verb::get, "/goober/ready");
  EXPECT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(parse(r.body)["checks"].as_object()["change_log"].as_object()["on"], false);
  std::string text;
  ASSERT_TRUE(host->content().put("playground", "x.md", "# x", &text)) << text;
  EXPECT_EQ(fetch(port, "localhost", bhttp::verb::get, "/x").status, 200);
}

// Databases (008 § 11): operators make one from stored schemas, snapshot it, and restore a
// snapshot to a new name over MCP; only a person drops anything, in the panel, after the host
// took a snapshot, and never while a box binds it.
TEST_F(PanelTest, OperatorsSnapshotAndCreateOnlyAPersonDrops) {
  std::string token = "0123456789abcdef0123";
  auto call = [&](const std::string& tool, const std::string& args) {
    auto r = mcp(token, R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool +
                            R"(","arguments":)" + args + "}}");
    EXPECT_EQ(r.status, 200) << r.body;
    auto result = parse(r.body)["result"].as_object();
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::value(bj::object{{"error", text}});
    std::string err;
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  };
  auto error_of = [](const bj::value& v) {
    return v.is_object() && v.as_object().contains("error") ? std::string(v.at("error").as_string()) : std::string();
  };
  ASSERT_EQ(error_of(call("schema_put", R"({"schema":{"schema":"shop1","tables":{"orders":{"columns":{
      "id":{"type":"id","primary-key":true},"paid":{"type":"boolean","default":false}}}}}})")), "");
  ASSERT_EQ(error_of(call("schema_put", R"({"schema":{"schema":"soak1","tables":{"soak_journal":{"columns":{
      "id":{"type":"id","primary-key":true},"note":{"type":"string"}}}}}})")), "");

  // A database made before any box binds it, empty.
  auto made = call("db_create", R"({"tenant":"playground","database":"shop","schemas":["shop1","soak1"]})");
  ASSERT_EQ(error_of(made), "") << json::dump(made);
  EXPECT_EQ(made.at("tables").as_array().size(), 2u);
  EXPECT_EQ(made.at("tables").at(0).at("rows"), 0);
  EXPECT_NE(error_of(call("db_create", R"({"tenant":"playground","database":"shop","schemas":["shop1"]})")).find("already"),
            std::string::npos);
  EXPECT_NE(error_of(call("db_create", R"({"tenant":"playground","database":"other","schemas":["nope1"]})")).find("isn't stored"),
            std::string::npos);

  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"shop","domains":["*"],
      "data":{"sqlite3":{"db":{"schema":"shop1","database":"shop","pattern":"sql","tables":{"orders":{"read":true}}}}},
      "routes":{"http":{"/x":{"get":{"name":"x","public":true}}}}})",
                                   R"((module (memory (export "memory") 1) (func (export "goober_route_x") (result i32) (i32.const 0))))",
                                   &err))
      << err;
  auto unbound = call("db_unbound", R"({"tenant":"playground"})");
  ASSERT_EQ(unbound.at("databases").as_array().size(), 1u) << json::dump(unbound);
  EXPECT_EQ(unbound.at("databases").at(0).at("bound_by").at(0).at("box"), "shop");
  EXPECT_EQ(unbound.at("databases").at(0).at("unbound_tables").at(0).at("table"), "soak_journal");
  EXPECT_EQ(unbound.at("unnamed_schemas").at(0), "soak1");

  // A snapshot, and a new database from it; never over an existing one.
  auto snap = call("db_snapshot", R"({"tenant":"playground","database":"shop","note":"before the move"})");
  ASSERT_EQ(error_of(snap), "") << json::dump(snap);
  std::string id(snap.at("snapshot").as_string());
  EXPECT_TRUE(id.starts_with("shop/")) << id;
  EXPECT_EQ(snap.at("sha256").as_string().size(), 64u);
  EXPECT_EQ(call("db_snapshots", R"({"tenant":"playground"})").at("snapshots").at(0).at("note"), "before the move");
  auto restored = call("db_restore_to_new", R"({"tenant":"playground","snapshot":")" + id + R"(","database":"shop_copy"})");
  ASSERT_EQ(error_of(restored), "") << json::dump(restored);
  EXPECT_EQ(restored.at("tables").as_array().size(), 2u);
  EXPECT_NE(error_of(call("db_restore_to_new", R"({"tenant":"playground","snapshot":")" + id + R"(","database":"shop"})"))
                .find("already has"),
            std::string::npos);
  EXPECT_NE(error_of(call("db_restore_to_new", R"({"tenant":"playground","snapshot":"shop/../../x","database":"y"})"))
                .find("no snapshot"),
            std::string::npos);
  // No tool drops anything.
  auto list = mcp(token, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").body;
  for (const char* t : {"db_drop", "table_drop", "schema_retire"}) EXPECT_EQ(list.find(t), std::string::npos) << t;

  // The panel: an observer can't; the owner types the name; bound things stay.
  std::string watcher = sign_in("watcher@example.com");
  EXPECT_EQ(api(watcher, "write/drop", R"({"kind":"database","tenant":"playground","database":"shop_copy","confirm":"shop_copy"})").status,
            403);
  std::string boss = sign_in("boss@example.com");
  ASSERT_FALSE(boss.empty());
  auto wrong = api(boss, "write/drop", R"({"kind":"table","tenant":"playground","database":"shop","table":"soak_journal","confirm":"soak_journal"})");
  EXPECT_EQ(wrong.status, 400) << wrong.body;
  auto table = api(boss, "write/drop",
                   R"({"kind":"table","tenant":"playground","database":"shop","table":"soak_journal","confirm":"shop.soak_journal","why":"the soak is over"})");
  ASSERT_EQ(table.status, 200) << table.body;
  auto before = std::string(parse(table.body)["result"].at("snapshot").as_string());
  EXPECT_EQ(call("db_describe", R"({"tenant":"playground","database":"shop"})").at("tables").as_array().size(), 1u);
  bool kept = false;
  auto shop_snaps = call("db_snapshots", R"({"tenant":"playground","database":"shop"})");
  for (const auto& s : shop_snaps.at("snapshots").as_array()) {
    kept |= s.at("snapshot").as_string() == before && s.as_object().contains("before_drop");
  }
  EXPECT_TRUE(kept) << before;
  auto bound = api(boss, "write/drop", R"({"kind":"database","tenant":"playground","database":"shop","confirm":"shop"})");
  EXPECT_EQ(bound.status, 409) << bound.body;
  EXPECT_NE(bound.body.find("shop.db"), std::string::npos) << bound.body;
  ASSERT_EQ(api(boss, "write/drop", R"({"kind":"database","tenant":"playground","database":"shop_copy","confirm":"shop_copy"})").status,
            200);
  EXPECT_EQ(call("db_describe", R"({"tenant":"playground","database":"shop_copy"})").at("exists"), false);

  // Retiring: refused while a box names the schema; a retired one can't be named again.
  EXPECT_EQ(api(boss, "write/drop", R"({"kind":"schema","schema":"shop1","confirm":"shop1"})").status, 409);
  ASSERT_EQ(api(boss, "write/drop", R"({"kind":"schema","schema":"soak1","confirm":"soak1"})").status, 200);
  EXPECT_FALSE(host->boxes().upload(R"({"box":"soak","domains":["*"],
      "data":{"sqlite3":{"db":{"schema":"soak1","database":"soak","pattern":"sql","tables":{"soak_journal":{"read":true}}}}},
      "routes":{"http":{"/y":{"get":{"name":"y","public":true}}}}})",
                                    R"((module (memory (export "memory") 1) (func (export "goober_route_y") (result i32) (i32.const 0))))",
                                    &err));
  EXPECT_NE(err.find("retired"), std::string::npos) << err;

  // A snapshot goes only when a person deletes it.
  ASSERT_EQ(api(boss, "write/drop", R"({"kind":"snapshot","tenant":"playground","snapshot":")" + id + R"(","confirm":")" + id + R"("})")
                .status,
            200);
  auto remaining = call("db_snapshots", R"({"tenant":"playground"})");
  for (const auto& s : remaining.at("snapshots").as_array()) EXPECT_NE(std::string(s.at("snapshot").as_string()), id);
}

// route_try in the panel, which only looks (and for an observer anywhere): an HTTP GET's try
// reads live data and is allowed; a write's try runs box code on copies, so it is refused.
TEST_F(PanelTest, AnObserverTriesGetsOnly) {
  const char* wat = R"wat((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22ok\22:true}")
  (func (export "goober_route_look") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 11)))
    (i32.const 0))
  (func (export "goober_route_poke") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 11)))
    (i32.const 0))
))wat";
  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"tryme","domains":["*"],"routes":{"http":{
      "/look":{"get":{"name":"look","public":true}},"/poke":{"post":{"name":"poke","public":true}}}}})",
                                   wat, &err))
      << err;
  std::string watcher = sign_in("watcher@example.com");
  ASSERT_FALSE(watcher.empty());
  auto look = api(watcher, "route_try", R"({"domain":"localhost","box":"tryme","path":"/look"})");
  EXPECT_EQ(look.status, 200) << look.body;
  EXPECT_NE(look.body.find("\"status\":200"), std::string::npos) << look.body;
  auto poke = api(watcher, "route_try", R"({"domain":"localhost","box":"tryme","path":"/poke","method":"post"})");
  EXPECT_EQ(poke.status, 403) << poke.body;
  auto tool = api(watcher, "route_try", R"({"domain":"localhost","box":"tryme","tool":"x"})");
  EXPECT_EQ(tool.status, 403) << tool.body;
  // The panel is look-only for an owner too.
  std::string boss = sign_in("boss@example.com");
  EXPECT_EQ(api(boss, "route_try", R"({"domain":"localhost","box":"tryme","path":"/poke","method":"post"})").status, 403);
}

// Schema evolution (design/machine/041.SchemaEvolution.md): a step that rewrites a table waits
// for a person in the panel; an upgrade box carries the rows over, first on copies (a failing row
// is named), then for real with a snapshot first, once no box pinned to the old version remains.
TEST_F(PanelTest, AForcedStepWaitsForAPersonAndAnUpgradeBox) {
  std::string token = "0123456789abcdef0123";
  auto call = [&](const std::string& tool, const std::string& args) {
    auto r = mcp(token, R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool +
                            R"(","arguments":)" + args + "}}");
    EXPECT_EQ(r.status, 200) << r.body;
    auto result = parse(r.body)["result"].as_object();
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::value(bj::object{{"error", text}});
    std::string err;
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  };
  auto error_of = [](const bj::value& v) {
    return v.is_object() && v.as_object().contains("error") ? std::string(v.at("error").as_string()) : std::string();
  };
  ASSERT_EQ(error_of(call("schema_put", R"({"schema":{"schema":"people1","tables":{"people":{"columns":{
      "name":{"type":"string"}}}}}})")), "");
  ASSERT_EQ(error_of(call("db_create", R"({"tenant":"playground","database":"people","schemas":["people1"]})")), "");
  ASSERT_EQ(error_of(call("db_exec", R"j({"tenant":"playground","database":"people",
      "sql":"INSERT INTO people (id, name) VALUES (1, 'Ada Lovelace'), (2, 'Grace Hopper')"})j")), "");
  const char* reader_wat = R"((module (memory (export "memory") 1) (func (export "goober_route_who") (result i32) (i32.const 0))))";
  auto reader = [](const char* schema) {
    return std::string(R"({"box":"reader","domains":["*"],"data":{"sqlite3":{"db":{"schema":")") + schema +
           R"(","database":"people","pattern":"sql","tables":{"people":{"read":true}}}}},)"
           R"("routes":{"http":{"/who":{"get":{"name":"who","public":true}}}}})";
  };
  std::string err;
  ASSERT_TRUE(host->boxes().upload(reader("people1"), reader_wat, &err)) << err;

  // name splits into first and last: forced, and it drops name, so it waits for a person.
  const char* people2 = R"({"schema":{"schema":"people2","follows":"people1","tables":{"people":{"columns":{
      "first":{"type":"string"},"last":{"type":"string"}}}}}})";
  auto check = call("schema_check", people2);
  ASSERT_EQ(error_of(check), "") << json::dump(check);
  EXPECT_EQ(check.at("held"), true);
  EXPECT_EQ(json::dump(check.at("step").at("forced_tables")), R"(["people"])");
  EXPECT_EQ(json::dump(check.at("databases").at(0).at("forced")), R"([{"table":"people","rows":2}])");
  EXPECT_EQ(json::dump(check.at("databases").at(0).at("drops")), R"([{"table":"people","column":"name","rows":2}])");
  ASSERT_EQ(error_of(call("schema_put", people2)), "");
  auto held = call("db_unbound", R"({"tenant":"playground"})").at("held_schemas");
  ASSERT_EQ(held.as_array().size(), 1u) << json::dump(held);
  EXPECT_EQ(held.at(0).at("schema"), "people2");
  EXPECT_FALSE(host->boxes().upload(reader("people2"), reader_wat, &err));
  EXPECT_NE(err.find("a person approves it"), std::string::npos) << err;

  // The upgrade box: one row's answer, so the second row fails its dry run.
  const char* upgrade_wat = R"wat((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22id\22:1,\22first\22:\22Ada\22,\22last\22:\22Lovelace\22}")
  (func (export "goober_route_split") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 40)))
    (i32.const 0))
))wat";
  ASSERT_TRUE(host->boxes().upload(R"({"box":"people_up","domains":["*"],
      "routes":{"upgrade":{"people":{"from":"people1.schema","to":"people2.schema","rows":{"name":"split"}}}}})",
                                   upgrade_wat, &err))
      << err;
  auto blocked = call("upgrade_dry_run", R"({"box":"people_up"})");
  EXPECT_NE(error_of(blocked).find("reader.db (people1)"), std::string::npos) << json::dump(blocked);

  // Only an owner approves, typing the name.
  std::string watcher = sign_in("watcher@example.com");
  EXPECT_EQ(api(watcher, "write/drop", R"({"kind":"schema_approve","schema":"people2","confirm":"people2"})").status, 403);
  std::string boss = sign_in("boss@example.com");
  EXPECT_EQ(api(boss, "write/drop", R"({"kind":"schema_approve","schema":"people2","confirm":"nope"})").status, 400);
  auto approved = api(boss, "write/drop", R"({"kind":"schema_approve","schema":"people2","confirm":"people2","why":"split names"})");
  ASSERT_EQ(approved.status, 200) << approved.body;
  EXPECT_EQ(call("db_unbound", R"({"tenant":"playground"})").at("held_schemas").as_array().size(), 0u);
  // The reader moves to people2: stored, and it waits for the upgrade.
  ASSERT_TRUE(host->boxes().upload(reader("people2"), reader_wat, &err)) << err;

  auto dry = call("upgrade_dry_run", R"({"box":"people_up"})");
  ASSERT_EQ(error_of(dry), "") << json::dump(dry);
  EXPECT_EQ(dry.at("passed"), false);
  EXPECT_EQ(dry.at("databases").at(0).at("rows"), 2);
  EXPECT_EQ(dry.at("databases").at(0).at("failures").at(0).at("key"), "2");
  EXPECT_NE(json::dump(dry).find("changed the primary key"), std::string::npos) << json::dump(dry);
  EXPECT_NE(error_of(call("upgrade_start", R"({"box":"people_up"})")).find("upgrade_dry_run first"), std::string::npos);
  // Nothing changed for real.
  EXPECT_EQ(call("db_query", R"({"tenant":"playground","database":"people","sql":"SELECT count(*) FROM people"})")
                .at("rows").at(0).at(0), 2);

  ASSERT_EQ(error_of(call("db_exec", R"({"tenant":"playground","database":"people","sql":"DELETE FROM people WHERE id = 2"})")), "");
  ASSERT_EQ(call("upgrade_dry_run", R"({"box":"people_up"})").at("passed"), true);
  auto start = call("upgrade_start", R"({"box":"people_up"})");
  ASSERT_EQ(error_of(start), "") << json::dump(start);
  EXPECT_EQ(start.at("databases").at(0).at("upgraded"), true) << json::dump(start);
  EXPECT_EQ(json::dump(call("db_query", R"({"tenant":"playground","database":"people","sql":"SELECT id, first, last FROM people"})")
                           .at("rows")),
            R"([[1,"Ada","Lovelace"]])");
  bool snapped = false;
  auto snaps = call("db_snapshots", R"({"tenant":"playground","database":"people"})");
  for (const auto& s : snaps.at("snapshots").as_array()) {
    snapped |= s.at("note") == "before schema people2";
  }
  EXPECT_TRUE(snapped);
  auto described = json::dump(call("db_describe", R"({"tenant":"playground","database":"people"})"));
  EXPECT_NE(described.find(R"("column":"name")"), std::string::npos) << described;
}

// A schema applies lazily, on the first call that opens the database; promote says which are
// pending, and schema_apply walks the line of versions now, in one transaction (041 § 6.1, § 8).
// An annotation shows in the step; a forced step on empty tables with no older writer needs no
// person, and one an older box still writes does.
TEST_F(PanelTest, SchemaApplyWalksTheLineAndPromoteSaysWhatsPending) {
  std::string token = "0123456789abcdef0123";
  auto call = [&](const std::string& tool, const std::string& args) {
    auto r = mcp(token, R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool +
                            R"(","arguments":)" + args + "}}");
    EXPECT_EQ(r.status, 200) << r.body;
    auto result = parse(r.body)["result"].as_object();
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::value(bj::object{{"error", text}});
    std::string err;
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  };
  auto error_of = [](const bj::value& v) {
    return v.is_object() && v.as_object().contains("error") ? std::string(v.at("error").as_string()) : std::string();
  };
  ASSERT_EQ(error_of(call("schema_put", R"({"schema":{"schema":"orders1","tables":{
      "orders":{"columns":{"id":{"type":"id","primary-key":true},"message_id":{"type":"string"}}},
      "tags":{"columns":{"id":{"type":"id","primary-key":true},"name":{"type":"string"}}}}}})")), "");
  ASSERT_EQ(error_of(call("db_create", R"({"tenant":"playground","database":"orders","schemas":["orders1"]})")), "");
  ASSERT_EQ(error_of(call("db_exec", R"j({"tenant":"playground","database":"orders",
      "sql":"INSERT INTO orders (message_id) VALUES ('m-1')"})j")), "");

  // v2: an annotation only.
  auto v2 = call("schema_put", R"({"schema":{"schema":"orders2","follows":"orders1","tables":{
      "orders":{"columns":{"id":{"type":"id","primary-key":true},"message_id":{"type":"string","not-a-reference":true}}},
      "tags":{"columns":{"id":{"type":"id","primary-key":true},"name":{"type":"string"}}}}}})");
  ASSERT_EQ(error_of(v2), "") << json::dump(v2);
  EXPECT_EQ(v2.at("step").at("changes").at(0).at("kind"), "annotation") << json::dump(v2);
  EXPECT_EQ(v2.at("held"), false);
  // v3: a required column with no default on tags, which is empty everywhere.
  const char* v3 = R"({"schema":{"schema":"orders3","follows":"orders2","tables":{
      "orders":{"columns":{"id":{"type":"id","primary-key":true},"message_id":{"type":"string","not-a-reference":true}}},
      "tags":{"columns":{"id":{"type":"id","primary-key":true},"name":{"type":"string"},"code":{"type":"string"}}}}}})";
  // While an older box writes tags, it waits for a person.
  const char* wat = R"((module (memory (export "memory") 1) (func (export "goober_route_r") (result i32) (i32.const 0))))";
  auto box = [](const char* name, const char* schema, const char* path) {
    return std::string(R"({"box":")") + name + R"(","domains":["*"],"data":{"sqlite3":{"db":{"schema":")" + schema +
           R"(","database":"orders","pattern":"sql","tables":{"tags":{"read":true,"insert":true}}}}},)"
           R"("routes":{"http":{")" + path + R"(":{"get":{"name":"r","public":true}}}}})";
  };
  std::string err;
  ASSERT_TRUE(host->boxes().upload(box("tagger", "orders1", "/t"), wat, &err)) << err;
  auto held = call("schema_check", v3);
  ASSERT_EQ(error_of(held), "") << json::dump(held);
  EXPECT_EQ(held.at("held"), true) << json::dump(held);
  ASSERT_EQ(error_of(call("box_disable", R"({"box":"tagger","why":"moving to orders4"})")), "");
  auto free = call("schema_put", v3);
  ASSERT_EQ(error_of(free), "") << json::dump(free);
  EXPECT_EQ(free.at("held"), false) << json::dump(free);
  EXPECT_NE(std::string(free.at("immediate").as_string()).find("empty"), std::string::npos);
  ASSERT_EQ(error_of(call("schema_put", R"({"schema":{"schema":"orders4","follows":"orders3","tables":{
      "orders":{"columns":{"id":{"type":"id","primary-key":true},"message_id":{"type":"string","not-a-reference":true},
                           "total":{"type":"int64","nullable":true}}},
      "tags":{"columns":{"id":{"type":"id","primary-key":true},"name":{"type":"string"},"code":{"type":"string"}}}}}})")),
            "");

  // A dry run walks v2, v3, v4 and changes nothing.
  auto dry = call("schema_apply", R"({"tenant":"playground","database":"orders","schema":"orders4","dry_run":true})");
  ASSERT_EQ(error_of(dry), "") << json::dump(dry);
  EXPECT_EQ(json::dump(dry.at("would_apply")), R"(["orders2","orders3","orders4"])");
  EXPECT_EQ(json::dump(dry.at("applied_after")), R"(["orders1"])");
  EXPECT_EQ(dry.as_object().count("snapshot"), 0u);

  // A box naming v4 goes live: nothing has opened the database, so v4 is pending, and promote says so.
  auto stored = host->boxes().store(box("shop", "orders4", "/s"), wat, "", &err);
  ASSERT_TRUE(stored) << err;
  auto promoted = call("version_promote", R"({"box":"shop","version":")" + stored->version + R"("})");
  ASSERT_EQ(error_of(promoted), "") << json::dump(promoted);
  bool pending = false;
  for (const auto& s : promoted.at("schemas").as_array()) {
    if (s.at("database") == "orders" && s.at("tenant") == "playground") {
      pending = s.at("state") == "pending";
      EXPECT_NE(std::string(s.at("says").as_string()).find("schema_apply"), std::string::npos);
    }
  }
  EXPECT_TRUE(pending) << json::dump(promoted);
  auto described = call("db_describe", R"({"tenant":"playground","database":"orders"})");
  EXPECT_EQ(json::dump(described.at("schemas")).find("orders4"), std::string::npos);

  // Applied now: the whole line, snapshot first, the forced step noted as needing no person.
  auto applied = call("schema_apply", R"({"tenant":"playground","database":"orders","schema":"orders4"})");
  ASSERT_EQ(error_of(applied), "") << json::dump(applied);
  EXPECT_EQ(json::dump(applied.at("applied_after")), R"(["orders1","orders2","orders3","orders4"])");
  EXPECT_EQ(applied.at("steps").as_array().size(), 3u);
  EXPECT_NE(json::dump(applied.at("steps").at(1)).find("without a person"), std::string::npos) << json::dump(applied);
  EXPECT_TRUE(applied.as_object().contains("snapshot")) << json::dump(applied);
  EXPECT_EQ(json::dump(applied.at("named_by")), R"(["shop.db"])");
  EXPECT_EQ(call("db_query", R"({"tenant":"playground","database":"orders","sql":"SELECT message_id FROM orders"})")
                .at("rows").at(0).at(0),
            "m-1");
  auto again = call("schema_apply", R"({"tenant":"playground","database":"orders","schema":"orders4"})");
  EXPECT_EQ(again.at("steps").as_array().size(), 0u);
  auto enabled = call("version_promote", R"({"box":"shop","version":")" + stored->version + R"("})");
  bool now_applied = false;
  for (const auto& s : enabled.at("schemas").as_array()) {
    if (s.at("database") == "orders" && s.at("tenant") == "playground") now_applied = s.at("state") == "applied";
  }
  EXPECT_TRUE(now_applied) << json::dump(enabled);
}

// A forced step on a table that holds rows still waits for a person, and schema_apply says why.
TEST_F(PanelTest, SchemaApplyRefusesAForcedStepOnRows) {
  std::string token = "0123456789abcdef0123";
  auto call = [&](const std::string& tool, const std::string& args) {
    auto r = mcp(token, R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool +
                            R"(","arguments":)" + args + "}}");
    auto result = parse(r.body)["result"].as_object();
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::value(bj::object{{"error", text}});
    std::string err;
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  };
  ASSERT_FALSE(call("schema_put", R"({"schema":{"schema":"kits1","tables":{"kits":{"columns":{
      "id":{"type":"id","primary-key":true},"name":{"type":"string"}}}}}})").as_object().contains("error"));
  ASSERT_FALSE(call("db_create", R"({"tenant":"playground","database":"kits","schemas":["kits1"]})").as_object().contains("error"));
  ASSERT_FALSE(call("db_exec", R"j({"tenant":"playground","database":"kits","sql":"INSERT INTO kits (name) VALUES ('a')"})j")
                   .as_object().contains("error"));
  auto put = call("schema_put", R"({"schema":{"schema":"kits2","follows":"kits1","tables":{"kits":{"columns":{
      "id":{"type":"id","primary-key":true},"name":{"type":"string"},"code":{"type":"string"}}}}}})");
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  EXPECT_EQ(put.at("held"), true);
  auto refused = call("schema_apply", R"({"tenant":"playground","database":"kits","schema":"kits2"})");
  ASSERT_TRUE(refused.as_object().contains("error"));
  EXPECT_NE(std::string(refused.at("error").as_string()).find("waits for a person"), std::string::npos)
      << json::dump(refused);
  EXPECT_EQ(json::dump(call("db_describe", R"({"tenant":"playground","database":"kits"})").at("schemas")).find("kits2"),
            std::string::npos);
}
