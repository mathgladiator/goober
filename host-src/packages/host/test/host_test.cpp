#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {
constexpr const char* kToken = "test-token-0123456789";

struct Reply {
  int status;
  std::string body;
  std::string etag;
  std::string content_type;
};

Reply fetch(uint16_t port, bhttp::verb verb, const std::string& target, const std::string& host = "localhost",
            const std::string& body = "", const std::string& auth = "", const std::string& if_none_match = "",
            const std::string& content_type = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, host);
  if (!auth.empty()) req.set(bhttp::field::authorization, auth);
  if (!if_none_match.empty()) req.set(bhttp::field::if_none_match, if_none_match);
  if (!content_type.empty()) req.set(bhttp::field::content_type, content_type);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  return {int(res.result_int()), res.body(), std::string(res[bhttp::field::etag]),
          std::string(res[bhttp::field::content_type])};
}

const char* kHelloWat = R"((module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_log" (func $log (param i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "hello was called")
  (func (export "goober_route_echo") (result i32)
    (local $n i32)
    (local.set $n (call $read (i32.const 1024) (call $size)))
    (drop (call $log (i32.const 1) (i32.const 16) (i32.const 16)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 1024) (local.get $n)))
    (i32.const 0))
  (func (export "goober_route_spin") (result i32) (loop $l (br $l)) (i32.const 0))
))";
}  // namespace

class HostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "host";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(
        R"({"listen":{"port":0},"storage":"storage","domains":{"localhost":{"tenant":"playground"}},
            "operations":{"token":")" + std::string(kToken) + R"("},"threads":{"network":1,"boxes":2},
            "limits":{"timeout-ms":{"default":200,"max":500}}})",
        root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(cfg);
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"},
                   {"id", 1},
                   {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp", "localhost", json::dump(req),
                   std::string("Bearer ") + kToken);
    EXPECT_EQ(r.status, 200) << r.body;
    std::string err;
    return *json::parse(r.body, &err);
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(HostTest, OperationsNeedTheToken) {
  auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp", "localhost", "{}");
  EXPECT_EQ(r.status, 401);
  r = fetch(port, bhttp::verb::post, "/goober-operations-mcp", "localhost", "{}", "Bearer wrong");
  EXPECT_EQ(r.status, 401);
  r = fetch(port, bhttp::verb::get, "/goober-operations-mcp", "localhost", "", std::string("Bearer ") + kToken);
  EXPECT_EQ(r.status, 405);
}

TEST_F(HostTest, StaticContentThroughMcpIsServedAndPersisted) {
  auto v = tool("content_put", {{"tenant", "playground"}, {"path", "index.html"}, {"text", "<h1>home</h1>"}});
  EXPECT_EQ(v.at("result").at("isError"), false) << json::dump(v);
  tool("content_put", {{"tenant", "playground"}, {"path", "img/dot.png"}, {"base64", "iVBORw0KGgo="}});
  auto r = fetch(port, bhttp::verb::get, "/");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body, "<h1>home</h1>");
  EXPECT_EQ(r.content_type, "text/html; charset=utf-8");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/", "localhost", "", "", r.etag).status, 304);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/img/dot.png").content_type, "image/png");
  EXPECT_EQ(*base::read_file(root / "storage/content/playground/index.html"), "<h1>home</h1>");

  auto list = tool("content_list", {{"tenant", "playground"}});
  EXPECT_NE(std::string(list.at("result").at("content").at(0).at("text").as_string()).find("img/dot.png"),
            std::string::npos);
  tool("content_delete", {{"tenant", "playground"}, {"path", "index.html"}});
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/").status, 404);
}

TEST_F(HostTest, ErrorPagesFallBackToUhOh) {
  auto r = fetch(port, bhttp::verb::get, "/missing");
  EXPECT_EQ(r.status, 404);
  EXPECT_NE(r.body.find("Uh Oh!"), std::string::npos);
  tool("content_put", {{"tenant", "_host"}, {"path", "error-message/404.html"}, {"text", "host 404"}});
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/missing").body, "host 404");
  tool("content_put", {{"tenant", "playground"}, {"path", "error-message/404.html"}, {"text", "tenant 404"}});
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/missing").body, "tenant 404");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/x", "unknown.example").status, 404);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/a/%2e%2e/b").status, 400);
}

TEST_F(HostTest, BoxesDeployAndAnswer) {
  bj::object manifest{{"box", "hello"},
                      {"domains", bj::array{"*"}},
                      {"behavior", bj::object{{"fuel", 2000000000}}},
                      {"routes", bj::object{{"http", bj::object{{"/echo/{thing : string}", bj::object{{"get", bj::object{{"name", "echo"}, {"public", true}}}}},
                                                                {"/spin", bj::object{{"get", bj::object{{"name", "spin"}, {"public", true}}}}}}}}}};
  auto v = tool("version_upload", {{"manifest", manifest}, {"wat", kHelloWat}});
  ASSERT_EQ(v.at("result").at("isError"), false) << json::dump(v);

  auto r = fetch(port, bhttp::verb::get, "/echo/world?x=1");
  ASSERT_EQ(r.status, 200) << r.body;
  std::string err;
  auto doc = json::parse(r.body, &err);
  ASSERT_TRUE(doc) << r.body;
  EXPECT_EQ(doc->at("params").at("thing"), "world");
  EXPECT_EQ(doc->at("query").at("x"), "1");
  EXPECT_EQ(doc->at("tenant"), "playground");

  EXPECT_EQ(fetch(port, bhttp::verb::get, "/spin").status, 504);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/echo/world").status, 405);

  auto logs = tool("box_logs", {{"box", "hello"}});
  std::string text(logs.at("result").at("content").at(0).at("text").as_string());
  EXPECT_NE(text.find("hello was called"), std::string::npos);
  EXPECT_NE(text.find("timeout"), std::string::npos);

  auto access = tool("access_log", {{"limit", 3}});
  EXPECT_NE(std::string(access.at("result").at("content").at(0).at("text").as_string()).find("\"box\""),
            std::string::npos);
}

TEST_F(HostTest, BadUploadsExplainThemselves) {
  auto v = tool("version_upload", {{"manifest", "{\"box\":\"x\"}"}, {"wat", "(module)"}});
  EXPECT_EQ(v.at("result").at("isError"), true);
  EXPECT_NE(std::string(v.at("result").at("content").at(0).at("text").as_string()).find("domains"),
            std::string::npos);
}

TEST_F(HostTest, FormsArriveDecodedAndAnonymousWritesAreBudgeted) {
  bj::object manifest{{"box", "hello"},
                      {"domains", bj::array{"*"}},
                      {"routes", bj::object{{"http", bj::object{{"/signup", bj::object{{"post", bj::object{{"name", "echo"}, {"public", true}}}}}}}}}};
  auto v = tool("version_upload", {{"manifest", manifest}, {"wat", kHelloWat}});
  ASSERT_EQ(v.at("result").at("isError"), false) << json::dump(v);
  auto r = fetch(port, bhttp::verb::post, "/signup", "localhost", "email=ada%40example.com&why=more+beef&x", "", "",
                 "application/x-www-form-urlencoded");
  ASSERT_EQ(r.status, 200) << r.body;
  std::string err;
  auto doc = json::parse(r.body, &err);
  ASSERT_TRUE(doc) << r.body;
  EXPECT_EQ(doc->at("form").at("email"), "ada@example.com");
  EXPECT_EQ(doc->at("form").at("why"), "more beef");
  // A stable pseudonymous key per address and site, never the address itself.
  EXPECT_EQ(doc->at("client_key").as_string().size(), 16u);
  EXPECT_EQ(r.body.find("127.0.0.1"), std::string::npos);
  // Anonymous writes have their own budget per address.
  int limited = 0;
  for (int i = 0; i < 15; ++i) limited += fetch(port, bhttp::verb::post, "/signup").status == 429;
  EXPECT_GT(limited, 0);
}

TEST_F(HostTest, OperatorsMaySendLargerBodiesThanAnyoneElse) {
  std::string big(3 << 20, 'x');  // past the 1 MiB default
  auto v = tool("content_put", {{"tenant", "playground"}, {"path", "big.txt"}, {"text", big}});
  ASSERT_EQ(v.at("result").at("isError"), false) << json::dump(v).substr(0, 300);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/big.txt").body.size(), big.size());
  // Without the token, the same body is refused before it is read: a 413, or the connection
  // closed while the client is still sending.
  auto refused = [&](const std::string& auth) {
    try {
      return fetch(port, bhttp::verb::post, "/goober-operations-mcp", "localhost", big, auth).status == 413;
    } catch (const std::exception&) {
      return true;
    }
  };
  EXPECT_TRUE(refused("Bearer wrong"));
  EXPECT_TRUE(refused(""));
}

// Mail templates through the operator tools (022): read the default, override it for one
// tenant, preview it, and see a bad one refused.
TEST_F(HostTest, MailTemplatesThroughMcp) {
  auto got = tool("mail_template_get", {{"tenant", "playground"}, {"name", "sign-in-new"}});
  auto& g = got.at("result").at("structuredContent");
  EXPECT_EQ(g.at("source"), "default");
  EXPECT_NE(std::string(g.at("html").as_string()).find("{{code}}"), std::string::npos);
  EXPECT_EQ(g.at("sample").at("code"), "042917");

  auto put = tool("mail_template_put", {{"tenant", "playground"},
                                        {"name", "sign-in-returning"},
                                        {"subject", "Howdy again, {{site}}"},
                                        {"text", "Your code: {{code}}"},
                                        {"html", ""}});
  EXPECT_FALSE(put.at("result").as_object().contains("isError") && put.at("result").at("isError").as_bool()) << json::dump(put);
  auto preview = tool("mail_template_preview", {{"tenant", "playground"}, {"name", "sign-in-returning"},
                                                {"vars", bj::object{{"code", "777777"}}}});
  auto& p = preview.at("result").at("structuredContent");
  EXPECT_EQ(p.at("subject"), "Howdy again, localhost");
  EXPECT_NE(std::string(p.at("html").as_string()).find("Your code: 777777"), std::string::npos);

  bool overridden = false;
  auto list = tool("mail_template_list", {{"tenant", "playground"}});
  for (const auto& t : list.at("result").at("structuredContent").at("templates").as_array()) {
    overridden |= t.at("name") == "sign-in-returning" && t.at("source") == "tenant";
  }
  EXPECT_TRUE(overridden);

  auto bad = tool("mail_template_put", {{"tenant", "playground"}, {"name", "layout"}, {"text", "no slot"}});
  EXPECT_TRUE(bad.at("result").at("isError").as_bool());
  auto gone = tool("mail_template_remove", {{"tenant", "playground"}, {"name", "sign-in-returning"}});
  EXPECT_FALSE(gone.at("result").as_object().contains("isError") && gone.at("result").at("isError").as_bool());
  auto unknown = tool("mail_template_list", {{"tenant", "nobody"}});
  EXPECT_TRUE(unknown.at("result").at("isError").as_bool());
  // Each call is a line in the access log: the tool, its tenant, and whether it failed.
  auto log = base::read_file(root / "storage/logs/access.log").value_or("");
  EXPECT_NE(log.find("\"tool\":\"mail_template_put\",\"arg_tenant\":\"playground\""), std::string::npos) << log.substr(0, 500);
  EXPECT_NE(log.find("\"tool\":\"mail_template_list\",\"arg_tenant\":\"nobody\",\"tool_error\":true"), std::string::npos);
}

// mcp_tools_preview: what a domain's /mcp lists, and for a person, what they see and why not.
TEST_F(HostTest, McpToolsPreviewShowsWhatAPersonSees) {
  const char* manifest = R"({"box":"loc","domains":["*"],"routes":{"mcp":{
    "list_locations":{"tool":{"description":"Lists.","requirements":[[]]}},
    "import_locations":{"tool":{"description":"Imports.","requirements":[["owner"]]}}}}})";
  const char* wat = R"((module (memory (export "memory") 1)
    (func (export "goober_route_list_locations") (result i32) (i32.const 0))
    (func (export "goober_route_import_locations") (result i32) (i32.const 0))))";
  std::string err;
  ASSERT_TRUE(host->boxes().upload(manifest, wat, &err)) << err;
  ASSERT_TRUE(host->identity().grant("playground", "helper@example.com", "staff", "test", 1'900'000'000'000, &err)) << err;
  auto all = tool("mcp_tools_preview", {{"domain", "localhost"}}).at("result").at("structuredContent");
  ASSERT_EQ(all.at("tools").as_array().size(), 2u);
  EXPECT_EQ(std::string(all.at("tools").at(0).at("version").as_string()), host->boxes().active()->boxes.at("loc")->version);
  auto as = tool("mcp_tools_preview", {{"domain", "localhost"}, {"email", "helper@example.com"}}).at("result").at("structuredContent");
  for (const auto& t : as.at("tools").as_array()) {
    if (t.at("name") == "list_locations") {
      EXPECT_TRUE(t.at("listed").as_bool());
    }
    if (t.at("name") == "import_locations") {
      EXPECT_FALSE(t.at("listed").as_bool());
      EXPECT_NE(std::string(t.at("why_not").as_string()).find("roles"), std::string::npos);
    }
  }
  EXPECT_TRUE(tool("mcp_tools_preview", {{"domain", "nope.example"}}).at("result").at("isError").as_bool());
}

TEST_F(HostTest, RouteCheckSaysWhoAnswersAPath) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(
      R"({"box":"places","domains":["*"],"routes":{"http":{"/places/{slug : string}":{"get":{"name":"place","public":true}}}}})",
      R"((module (memory (export "memory") 1) (func (export "goober_route_place") (result i32) (i32.const 0))))", &err))
      << err;
  ASSERT_TRUE(host->boxes().upload(
      R"({"box":"admin","domains":["*"],"routes":{"http":{"/places/new":{"get":{"name":"new_place","requirements":[["owner"]]}}}}})",
      R"((module (memory (export "memory") 1) (func (export "goober_route_new_place") (result i32) (i32.const 0))))", &err))
      << err;
  // The same verb and shape in a second box is refused at upload, never shadowed.
  EXPECT_FALSE(host->boxes().upload(
      R"({"box":"rival","domains":["*"],"routes":{"http":{"/places/{name : string}":{"get":{"name":"rival_place","public":true}}}}})",
      R"((module (memory (export "memory") 1) (func (export "goober_route_rival_place") (result i32) (i32.const 0))))", &err));
  EXPECT_NE(err.find("conflicts with another box"), std::string::npos) << err;

  auto check = [&](const std::string& path, const std::string& verb = "get") {
    return tool("route_check", {{"domain", "localhost"}, {"path", path}, {"verb", verb}}).at("result").at("structuredContent");
  };
  // A literal beats a parameter across boxes, and the loser is listed.
  auto lit = check("/places/new");
  EXPECT_EQ(lit.at("served_by"), "box");
  EXPECT_EQ(lit.at("route").at("box"), "admin");
  EXPECT_EQ(lit.at("route").at("route"), "new_place");
  ASSERT_EQ(lit.at("also_fits").as_array().size(), 1u);
  EXPECT_EQ(lit.at("also_fits").at(0).at("box"), "places");
  // The query doesn't choose the route; it comes back parsed.
  auto with_query = check("/places/joes?tag=dirty-carnivore#top");
  EXPECT_EQ(with_query.at("route").at("box"), "places");
  EXPECT_EQ(with_query.at("path"), "/places/joes");
  EXPECT_EQ(with_query.at("query").at("tag"), "dirty-carnivore");
  auto param = check("/places/joes");
  EXPECT_EQ(param.at("route").at("box"), "places");
  EXPECT_EQ(param.at("route").at("params").at("slug"), "joes");
  EXPECT_EQ(check("/places/joes", "post").at("status"), 405);
  EXPECT_EQ(check("/identity/sign-in").at("served_by"), "host");
  EXPECT_EQ(check("/nothing-here").at("status"), 404);
  EXPECT_TRUE(tool("route_check", {{"domain", "nope.example"}, {"path", "/"}}).at("result").at("isError").as_bool());
}

// A redirect domain (015.Domains.md) answers everything itself: the primary domain, path and
// query kept, 301 for reads and 308 for the rest; never a page, a box, or a sign-in.
TEST(HostRedirects, ARedirectDomainSendsEverythingToItsPrimary) {
  auto root = fs::path(GOOBER_TEST_TMP) / "host_redirects";
  fs::remove_all(root);
  std::string err;
  auto cfg = config::parse_config(
      R"({"listen":{"port":0},"storage":"storage","threads":{"network":1,"boxes":1},
          "operations":{"token":")" + std::string(kToken) + R"("},
          "domains":{"example.com":{"tenant":"shop"},"www.example.com":{"redirect-to":"example.com"}}})",
      root, &err);
  ASSERT_TRUE(cfg) << err;
  host::Host h(cfg);
  auto port = h.start();
  auto send = [&](bhttp::verb verb, const std::string& target, const std::string& host_header) {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    bhttp::request<bhttp::string_body> req(verb, target, 11);
    req.set(bhttp::field::host, host_header);
    req.prepare_payload();
    bhttp::write(stream, req);
    beast::flat_buffer buf;
    bhttp::response<bhttp::string_body> res;
    bhttp::read(stream, buf, res);
    return std::pair{int(res.result_int()), std::string(res[bhttp::field::location])};
  };
  EXPECT_EQ(send(bhttp::verb::get, "/menu/today?x=1&y=2", "WWW.example.com"),
            (std::pair{301, std::string("https://example.com/menu/today?x=1&y=2")}));
  EXPECT_EQ(send(bhttp::verb::get, "/", "www.example.com:443"), (std::pair{301, std::string("https://example.com/")}));
  EXPECT_EQ(send(bhttp::verb::post, "/identity/sign-in", "www.example.com"),
            (std::pair{308, std::string("https://example.com/identity/sign-in")}));
  // The path is only ever a path on the primary: an absolute target can't send people elsewhere.
  EXPECT_EQ(send(bhttp::verb::get, "http://evil.example/x", "www.example.com").second,
            "https://example.com/http://evil.example/x");
  // The primary is served as before, and the proxy's health check answers whatever the Host.
  EXPECT_NE(send(bhttp::verb::get, "/", "example.com").first, 301);
  EXPECT_EQ(send(bhttp::verb::get, "/goober/health", "www.example.com").first, 200);

  // Operators see it: route_check says the host answers, domain_list names where it goes.
  auto tool = [&](const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp", "example.com", json::dump(req),
                   std::string("Bearer ") + kToken);
    std::string e;
    auto v = json::parse(r.body, &e);
    EXPECT_TRUE(v && v->as_object().contains("result")) << r.status << " " << r.body;
    // An object comes back structured; a list only as the content's text.
    const auto& result = v->at("result").as_object();
    if (const auto* sc = result.if_contains("structuredContent")) return *sc;
    return *json::parse(std::string(result.at("content").at(0).at("text").as_string()), &e);
  };
  auto check = tool("route_check", {{"domain", "www.example.com"}, {"path", "/menu"}});
  EXPECT_EQ(check.at("served_by"), "host");
  EXPECT_EQ(check.at("redirect_to"), "example.com");
  EXPECT_EQ(check.at("tenant"), "shop");
  auto list = tool("domain_list", {}).as_array();
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list.at(1).at("redirect_to"), "example.com");
}
