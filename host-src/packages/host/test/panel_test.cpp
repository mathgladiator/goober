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
  for (const char* k : {"storage", "identity", "boxes", "mcp", "change_log", "traffic", "queue"}) {
    EXPECT_TRUE(ready["checks"].as_object().contains(k)) << k;
  }
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
