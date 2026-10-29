// End to end: typed requests and responses (036, 037) and /mcp for agents (050);
// design/machine/011.Types.md and 012.Agents.md.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

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

Reply fetch(uint16_t port, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "localhost");
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

// echo returns the request document; good and bad return fixed JSON.
const char* kWat = R"wat((module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\"n\":7}")
  (data (i32.const 64) "{\"n\":7,\"leak\":\"secret\"}")
  (func $echo (result i32)
    (local $n i32)
    (local.set $n (call $read (i32.const 1024) (call $size)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 1024) (local.get $n)))
    (i32.const 0))
  (func (export "goober_route_add") (result i32) (call $echo))
  (func (export "goober_route_list") (result i32) (call $echo))
  (func (export "goober_route_good") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 7))) (i32.const 0))
  (func (export "goober_route_bad") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 64) (i32.const 23))) (i32.const 0))
  (func (export "goober_route_add_order") (result i32) (call $echo))
  (func (export "goober_route_admin_only") (result i32) (call $echo))
))wat";

const char* kManifest = R"({"box":"orders","domains":["*"],"types":"orders.openapi.json","routes":{
  "http":{
    "/orders":{"post":{"name":"add","requirements":[["owner"]],"agents":true,"body":"json:Order","max-body-bytes":200,
                       "request-headers":["idempotency-key"]},
               "get":{"name":"list","public":true,"query":{"limit":{"type":"integer","format":"int32","default":10,"maximum":50},
                                                          "q":{"type":"string","maxLength":8}}}},
    "/good":{"get":{"name":"good","public":true,"response":"json:Order"}},
    "/bad":{"get":{"name":"bad","public":true,"response":"json:Order"}}},
  "mcp":{
    "add_order":{"tool":{"description":"Adds an order.","input":"json:Order","requirements":[["owner"]]}},
    "admin_only":{"tool":{"description":"Only admins.","requirements":[["admin"]]}}}}})";

const char* kTypes = R"({"openapi":"3.1.0","components":{"schemas":{
  "Order":{"type":"object","required":["n"],"additionalProperties":false,"properties":{
    "n":{"type":"integer","format":"int32","minimum":1},
    "id":{"type":"integer","format":"int64"},
    "wrap":{"type":"boolean","default":false}}}}}})";

}  // namespace

class AgentsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "agents";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    auto& id = host->identity();
    int64_t now = 1'800'000'000'000;
    ASSERT_TRUE(id.grant("playground", "exec@example.com", "owner", "test", now, &err)) << err;
    std::string uid(id.describe("playground", now).at("users").at(0).at("user_id").as_string());
    int64_t user = std::stoll(uid);
    auto agent = id.register_client("playground", "Grok", {}, now, &err);
    ASSERT_TRUE(agent) << err;
    identity::OAuthError e;
    auto d = id.start_device("playground", agent->client_id, base::now_unix_ms(), &e);
    ASSERT_TRUE(d);
    ASSERT_TRUE(id.decide_device("playground", d->user_code, user, true, base::now_unix_ms(), &err));
    auto t = id.poll_device("playground", agent->client_id, d->device_code, base::now_unix_ms(), 86400000, &e);
    ASSERT_TRUE(t) << e.description;
    agent_token = t->access_token;
    ASSERT_TRUE(host->boxes().upload(kManifest, kWat, kTypes, &err)) << err;
  }
  void TearDown() override { host.reset(); }

  bj::value rpc(const std::string& method, bj::object params, int* status = nullptr) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", std::move(params)}};
    auto r = fetch(port, bhttp::verb::post, "/mcp",
                   {{"authorization", "Bearer " + agent_token}, {"content-type", "application/json"}}, json::dump(req));
    if (status) *status = r.status;
    std::string err;
    auto v = json::parse(r.body, &err);
    return v ? *v : bj::value(r.body);
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string agent_token;
};

TEST_F(AgentsTest, TypedRequestsNeverReachTheBoxInvalid) {
  std::vector<std::pair<std::string, std::string>> api = {{"authorization", "Bearer " + agent_token},
                                                          {"content-type", "application/json"},
                                                          {"idempotency-key", "k1"},
                                                          {"x-other", "hidden"}};
  auto ok = fetch(port, bhttp::verb::post, "/orders", api, R"({"n": 3, "id": 9007199254740993})");
  ASSERT_EQ(ok.status, 200) << ok.body;
  std::string err;
  auto doc = json::parse(ok.body, &err)->as_object();
  // Defaults filled, int64 as a string, and only the declared header.
  EXPECT_EQ(doc.at("body"), R"({"n":3,"id":"9007199254740993","wrap":false})");
  EXPECT_EQ(doc.at("headers").as_object().size(), 1u);
  EXPECT_EQ(doc.at("headers").at("idempotency-key"), "k1");

  auto refused = [&](const std::string& body, int status, const char* why, const char* type = "application/json") {
    auto h = api;
    h[1].second = type;
    auto r = fetch(port, bhttp::verb::post, "/orders", h, body);
    EXPECT_EQ(r.status, status) << body << " " << r.body;
    EXPECT_NE(r.body.find(why), std::string::npos) << r.body;
  };
  refused(R"({"n": 0})", 400, "$.n: must be at least 1");
  refused(R"({"n": 1, "evil": true})", 400, "objects are closed");
  refused(R"({})", 400, "$.n: is required");
  refused("not json", 400, "isn't JSON");
  refused(R"({"n":1})", 415, "application/json", "text/plain");
  refused(R"({"n":1,"id":")" + std::string(300, '1') + "\"}", 413, "larger than 200");

  auto list = fetch(port, bhttp::verb::get, "/orders?limit=5&junk=1");
  ASSERT_EQ(list.status, 200);
  auto q = json::parse(list.body, &err)->at("query").as_object();
  EXPECT_EQ(q.at("limit"), 5);  // typed, not a string
  EXPECT_FALSE(q.contains("junk"));
  EXPECT_EQ(json::parse(fetch(port, bhttp::verb::get, "/orders").body, &err)->at("query").at("limit"), 10);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/orders?limit=99").status, 400);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/orders?q=waytoolongforit").status, 400);

  // A box's response must match its type: no leaked fields.
  auto good = fetch(port, bhttp::verb::get, "/good");
  EXPECT_EQ(good.body, R"({"n":7,"wrap":false})");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/bad").status, 500);
  auto log = base::read_file(root / "storage/logs/boxes/orders.log");
  EXPECT_NE(log->find("response isn't a valid Order"), std::string::npos);
}

TEST_F(AgentsTest, AgentsCallBoxToolsOverMcp) {
  // Without a token: 401 naming the protected-resource metadata, which names the realm.
  auto anon = fetch(port, bhttp::verb::post, "/mcp", {{"content-type", "application/json"}}, "{}");
  EXPECT_EQ(anon.status, 401);
  EXPECT_NE(anon.headers["www-authenticate"].find("/.well-known/oauth-protected-resource"), std::string::npos);
  auto prm = fetch(port, bhttp::verb::get, "/.well-known/oauth-protected-resource");
  EXPECT_NE(prm.body.find(R"("authorization_servers":["http://localhost"])"), std::string::npos) << prm.body;

  auto init = rpc("initialize", {{"protocolVersion", "2025-06-18"}});
  EXPECT_EQ(init.at("result").at("protocolVersion"), "2025-06-18");
  auto list = rpc("tools/list", {});
  const auto& tools = list.at("result").at("tools").as_array();
  ASSERT_EQ(tools.size(), 1u);  // admin_only is hidden from someone who isn't an admin
  EXPECT_EQ(tools[0].at("name"), "add_order");
  EXPECT_EQ(tools[0].at("inputSchema").at("required").at(0), "n");
  // Each tool says which box version answers it, and the server's version names the tool set,
  // so it changes when a deploy changes what this caller sees.
  std::string version(host->boxes().active()->boxes.at("orders")->version);
  EXPECT_EQ(std::string(tools[0].at("_meta").at("goober/version").as_string()), version);
  EXPECT_EQ(tools[0].at("_meta").at("goober/box"), "orders");
  std::string server_version(init.at("result").at("serverInfo").at("version").as_string());
  std::string err2;
  std::string changed = std::string(kManifest);
  changed.replace(changed.find("Adds an order."), 14, "Adds an order!");
  ASSERT_TRUE(host->boxes().upload(changed, kWat, kTypes, &err2)) << err2;
  auto again = rpc("initialize", {{"protocolVersion", "2025-06-18"}});
  EXPECT_NE(std::string(again.at("result").at("serverInfo").at("version").as_string()), server_version);
  auto relisted = rpc("tools/list", {});
  EXPECT_EQ(relisted.at("result").at("tools").at(0).at("description"), "Adds an order!");  // no cache: the new version

  auto call = rpc("tools/call", {{"name", "add_order"}, {"arguments", bj::object{{"n", 2}}}});
  ASSERT_TRUE(call.at("result").at("isError").is_bool()) << json::dump(call);
  EXPECT_FALSE(call.at("result").at("isError").as_bool()) << json::dump(call);
  const auto& doc = call.at("result").at("structuredContent");
  EXPECT_EQ(doc.at("method"), "tool");
  EXPECT_EQ(doc.at("body"), R"({"n":2,"wrap":false})");
  EXPECT_EQ(doc.at("user").at("agent"), true);

  auto bad = rpc("tools/call", {{"name", "add_order"}, {"arguments", bj::object{{"n", "two"}}}});
  EXPECT_TRUE(bad.at("result").at("isError").as_bool());
  EXPECT_NE(json::dump(bad).find("must be an int32"), std::string::npos);
  auto hidden = rpc("tools/call", {{"name", "admin_only"}, {"arguments", bj::object{}}});
  EXPECT_NE(json::dump(hidden).find("unknown tool"), std::string::npos);  // not found, never forbidden

  // The person sees it on their account page.
  fetch(port, bhttp::verb::post, "/identity/email/start", {{"content-type", "application/x-www-form-urlencoded"}},
        "email=exec%40example.com");
  host->drain_mail();
  auto files = base::list_files(root / "storage/mail/outbox");
  std::sort(files.begin(), files.end());
  auto mail = *base::read_file(root / "storage/mail/outbox" / files.back());
  std::string code = mail.substr(mail.find(" is ") + 4, 6);
  auto verify = fetch(port, bhttp::verb::post, "/identity/email/verify",
                      {{"content-type", "application/x-www-form-urlencoded"}}, "email=exec%40example.com&code=" + code);
  std::string cookie = verify.headers["set-cookie"].substr(0, verify.headers["set-cookie"].find(';'));
  auto account = fetch(port, bhttp::verb::get, "/identity/account", {{"cookie", cookie}});
  EXPECT_NE(account.body.find("What your agents did"), std::string::npos) << account.body.substr(0, 400);
  EXPECT_NE(account.body.find("<code>add_order</code> by Grok"), std::string::npos);
  auto audit = base::read_file(root / "storage/logs/agents.log");
  ASSERT_TRUE(audit);
  EXPECT_NE(audit->find(R"("tool":"add_order")"), std::string::npos);
  EXPECT_NE(audit->find(R"("agent":true)"), std::string::npos);
}

TEST_F(AgentsTest, UploadsCheckTheTypesDocument) {
  std::string err;
  EXPECT_FALSE(host->boxes().upload(kManifest, kWat, &err));
  EXPECT_NE(err.find("wasn't supplied"), std::string::npos) << err;
  EXPECT_FALSE(host->boxes().upload(kManifest, kWat, R"({"openapi":"3.1.0","components":{"schemas":{}}})", &err));
  EXPECT_NE(err.find("isn't in orders.openapi.json"), std::string::npos) << err;
  EXPECT_FALSE(host->boxes().upload(kManifest, kWat,
                                    R"({"openapi":"3.1.0","components":{"schemas":{"Order":{"type":"integer"}}}})", &err));
  EXPECT_NE(err.find("int32 or int64"), std::string::npos) << err;
}

TEST_F(AgentsTest, OperatorsMintLongLivedAgentTokens) {
  auto& id = host->identity();
  std::string err;
  int64_t now = base::now_unix_ms();
  identity::Client grok;
  grok.client_id = "grokbot";
  grok.name = "GrokBot";
  grok.person = false;
  grok.source = "operator";
  ASSERT_TRUE(id.put_client("playground", grok, now, &err)) << err;
  identity::Client phone = grok;
  phone.client_id = "phone";
  phone.person = true;
  phone.redirect_uris = {"http://127.0.0.1/cb"};
  ASSERT_TRUE(id.put_client("playground", phone, now, &err)) << err;
  int64_t day = 24LL * 3600 * 1000;
  EXPECT_FALSE(id.mint_agent_token("playground", "phone", "exec@example.com", 30 * day, now, &err));
  EXPECT_FALSE(id.mint_agent_token("playground", "grokbot", "nobody@example.com", 30 * day, now, &err));
  EXPECT_FALSE(id.mint_agent_token("playground", "grokbot", "exec@example.com", 400 * day, now, &err));
  auto token = id.mint_agent_token("playground", "grokbot", "exec@example.com", 30 * day, now, &err);
  ASSERT_TRUE(token) << err;
  // It works on /mcp as an agent, well past the hour an OAuth access token lives.
  auto b = id.bearer("playground", *token, now + 20 * day);
  ASSERT_TRUE(b);
  EXPECT_TRUE(b->agent);
  agent_token = *token;
  auto list = rpc("tools/list", {});
  EXPECT_EQ(list.at("result").at("tools").as_array().size(), 1u) << json::dump(list);
  // The person sees it as a grant and can end it.
  auto grants = id.grants("playground", b->user.user_id, now);
  std::string family;
  for (const auto& g : grants) {
    if (g.at("client_id") == "grokbot") family = std::string(g.at("grant").as_string());
  }
  ASSERT_FALSE(family.empty()) << json::dump(grants);
  ASSERT_TRUE(id.revoke_grant("playground", b->user.user_id, family, now));
  EXPECT_FALSE(id.bearer("playground", *token, now));
  int status = 0;
  rpc("tools/list", {}, &status);
  EXPECT_EQ(status, 401);
}
