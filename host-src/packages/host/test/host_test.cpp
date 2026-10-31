#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/files.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"
#include "goober/ids/ids.h"
#include "goober/sqlite/sqlite.h"
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

// A version stored dark (activate: false) serves nothing until promoted; a disabled box serves
// nothing until enabled, and both show in version_list and the change log.
TEST_F(HostTest, DarkVersionsAndDisabledBoxes) {
  auto sc = [](const bj::value& v) { return v.at("result").at("structuredContent"); };
  auto manifest = [](const char* path) {
    return bj::object{{"box", "hello"},
                      {"domains", bj::array{"*"}},
                      {"routes", bj::object{{"http", bj::object{{path, bj::object{{"get", bj::object{{"name", "echo"}, {"public", true}}}}}}}}}};
  };
  auto first = tool("version_upload", {{"manifest", manifest("/one/{thing : string}")}, {"wat", kHelloWat}});
  ASSERT_EQ(first.at("result").at("isError"), false) << json::dump(first);
  std::string v1(sc(first).at("version").as_string());
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/one/x").status, 200);

  // Stored dark: every check ran, the old version still answers.
  auto dark = tool("version_upload", {{"manifest", manifest("/two/{thing : string}")}, {"wat", kHelloWat}, {"activate", false}});
  ASSERT_EQ(dark.at("result").at("isError"), false) << json::dump(dark);
  EXPECT_EQ(sc(dark).at("active"), false);
  std::string v2(sc(dark).at("version").as_string());
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/one/x").status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/two/x").status, 404);
  // Checks still run in the dark: a broken module is refused.
  auto broken = tool("version_upload", {{"manifest", manifest("/three")}, {"wat", "(module"}, {"activate", false}});
  EXPECT_EQ(broken.at("result").at("isError"), true);
  ASSERT_EQ(tool("version_promote", {{"box", "hello"}, {"version", v2}}).at("result").at("isError"), false);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/two/x").status, 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/one/x").status, 404);

  // Disabled: nothing answers, the versions stay, and enabling brings back the same version.
  EXPECT_EQ(tool("box_disable", {{"box", "hello"}}).at("result").at("isError"), true);  // why is required
  auto off = tool("box_disable", {{"box", "hello"}, {"why", "it mails the wrong people"}});
  ASSERT_EQ(off.at("result").at("isError"), false) << json::dump(off);
  EXPECT_EQ(sc(off).at("version").as_string(), v2);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/two/x").status, 404);
  auto list = tool("version_list", {{"box", "hello"}});
  std::string listed(list.at("result").at("content").at(0).at("text").as_string());
  EXPECT_NE(listed.find("\"disabled\": true"), std::string::npos) << listed;
  EXPECT_NE(listed.find("wrong people"), std::string::npos) << listed;
  EXPECT_EQ(listed.find("\"active\": true"), std::string::npos) << listed;
  auto changes = tool("changes", {{"kind", "deploy"}});
  EXPECT_NE(json::dump(changes).find("disable"), std::string::npos) << json::dump(changes);
  // The box is gone from service across a restart too.
  host.reset();
  {
    std::string err;
    auto cfg = config::parse_config(
        R"({"listen":{"port":0},"storage":"storage","domains":{"localhost":{"tenant":"playground"}},
            "operations":{"token":")" + std::string(kToken) + R"("},"threads":{"network":1,"boxes":2},
            "limits":{"timeout-ms":{"default":200,"max":500}}})",
        root, &err);
    host = std::make_unique<host::Host>(cfg);
    port = host->start();
  }
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/two/x").status, 404);
  auto on = tool("box_enable", {{"box", "hello"}});
  ASSERT_EQ(on.at("result").at("isError"), false) << json::dump(on);
  EXPECT_EQ(sc(on).at("active").as_string(), v2);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/two/x").status, 200);
  // Enabled again, it isn't disabled any more: enabling twice says so.
  EXPECT_EQ(tool("box_enable", {{"box", "hello"}}).at("result").at("isError"), true);
  (void)v1;
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

// The id service in the host (design/machine/038.Ids.md): its first start jumps past every id
// already stored, users take their ids from it, and a restore moves it past again.
TEST(HostIds, NewIdsStartPastStoredOnesAndUsersTakeThem) {
  auto root = fs::path(GOOBER_TEST_TMP) / "host_ids";
  fs::remove_all(root);
  // A database from before, holding a large id.
  fs::create_directories(root / "storage" / "sqlite3" / "shop" / "databases");
  {
    sqlite::Db old(root / "storage" / "sqlite3" / "shop" / "databases" / "main.db");
    old.exec("CREATE TABLE things (id INTEGER PRIMARY KEY, name TEXT)");
    old.exec("INSERT INTO things VALUES (" + std::to_string(ids::kFirstId + 500) + ", 'old')");
  }
  std::string err;
  auto cfg = config::parse_config(
      R"({"listen":{"port":0},"storage":"storage","threads":{"network":1,"boxes":1},
          "operations":{"token":")" + std::string(kToken) + R"("},"domains":{"localhost":{"tenant":"shop"}}})",
      root, &err);
  ASSERT_TRUE(cfg) << err;
  int64_t past = ids::kFirstId + 500 + ids::kMargin;
  {
    host::Host h(cfg);
    auto port = h.start();
    auto tool = [&](const std::string& name, bj::object args) {
      bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                     {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
      auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp", "localhost", json::dump(req),
                     std::string("Bearer ") + kToken);
      std::string e;
      return json::parse(r.body, &e)->at("result").at("structuredContent");
    };
    auto fresh = tool("ids_status", {});
    EXPECT_EQ(fresh.at("counter").as_string(), std::to_string(past));
    // Nothing minted yet: the answer says what that means, rather than looking broken.
    EXPECT_NE(std::string(fresh.at("note").as_string()).find("no id minted yet"), std::string::npos);
    tool("role_grant", {{"tenant", "shop"}, {"email", "ada@example.com"}, {"role", "member"}});
    tool("role_grant", {{"tenant", "shop"}, {"email", "bo@example.com"}, {"role", "member"}});
    auto users = tool("realm_get", {{"tenant", "shop"}}).at("users").as_array();
    ASSERT_EQ(users.size(), 2u);
    EXPECT_EQ(users[0].at("user_id").as_string(), std::to_string(past));
    EXPECT_EQ(users[1].at("user_id").as_string(), std::to_string(past + 1));
    auto status = tool("ids_status", {});
    EXPECT_EQ(status.at("counter").as_string(), std::to_string(past + ids::kRangeLength));
    EXPECT_EQ(status.at("next").as_string(), std::to_string(past + 2));
    EXPECT_FALSE(status.as_object().contains("note"));
  }
  // A restore: the counter moves past the restored ids and a margin beyond where it was.
  auto [from, to] = host::move_ids_past_restore(root / "storage");
  EXPECT_EQ(from, past + ids::kRangeLength);
  EXPECT_EQ(to, past + ids::kRangeLength + ids::kMargin);
}

// Box flags (design/machine/039.Flags.md): a box reads a declared flag's default, an operator
// flips it with flag_set and the very next call sees it (no new version), an undeclared name
// reads -1, values outside the declared set are refused, the change log and audit log keep the
// flip, and a restart keeps the value.
TEST(HostFlags, OperatorsFlipFlagsBoxesReadThem) {
  const char* kFlagWat = R"((module
  (import "goober" "goober_flag" (func $flag (param i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "text/plain")
  (data (i32.const 16) "frozen")
  (data (i32.const 32) "nope")
  (data (i32.const 48) "undeclared")
  (func (export "goober_route_read_flag") (result i32)
    (local $n i32)
    (local.set $n (call $flag (i32.const 16) (i32.const 6)))
    (if (i32.lt_s (local.get $n) (i32.const 0))
      (then (drop (call $respond (i32.const 404) (i32.const 0) (i32.const 10) (i32.const 48) (i32.const 10))))
      (else (drop (call $result (i32.const 1024) (local.get $n)))
            (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 10) (i32.const 1024) (local.get $n)))))
    (i32.const 0))
  (func (export "goober_route_read_missing") (result i32)
    (drop (call $respond (select (i32.const 204) (i32.const 500) (i32.eq (call $flag (i32.const 32) (i32.const 4)) (i32.const -1)))
                         (i32.const 0) (i32.const 10) (i32.const 0) (i32.const 0)))
    (i32.const 0))
))";
  const char* kFlagManifest = R"({"box":"switches","domains":["*"],
    "flags":{"frozen":{"default":"false","values":["false","true"],"about":"No new orders while true."}},
    "routes":{"http":{"/frozen":{"get":{"name":"read_flag","public":true}},
                      "/missing":{"get":{"name":"read_missing","public":true}}}}})";
  auto root = fs::path(GOOBER_TEST_TMP) / "host_flags";
  fs::remove_all(root);
  std::string err;
  auto cfg = config::parse_config(
      R"({"listen":{"port":0},"storage":"storage","threads":{"network":1,"boxes":1},
          "operations":{"token":")" + std::string(kToken) + R"("},"domains":{"localhost":{"tenant":"shop"}}})",
      root, &err);
  ASSERT_TRUE(cfg) << err;
  auto call = [&](uint16_t port, const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp", "localhost", json::dump(req),
                   std::string("Bearer ") + kToken);
    std::string e;
    auto doc = json::parse(r.body, &e);
    EXPECT_TRUE(doc) << r.status << " " << r.body;
    if (!doc) return bj::value(bj::object{{"error", r.body}});
    const auto& result = doc->at("result").as_object();
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::value(bj::object{{"error", text}});
    return *json::parse(text, &e);
  };
  {
    host::Host h(cfg);
    auto port = h.start();
    ASSERT_TRUE(h.boxes().upload(kFlagManifest, kFlagWat, &err)) << err;
    auto first = fetch(port, bhttp::verb::get, "/frozen");
    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(first.body, "false");  // the default
    EXPECT_EQ(fetch(port, bhttp::verb::get, "/missing").status, 204);  // an undeclared name reads -1

    auto refused = call(port, "flag_set", {{"tenant", "shop"}, {"box", "switches"}, {"flag", "frozen"}, {"value", "maybe"},
                                           {"reason", "testing"}});
    EXPECT_NE(json::dump(refused).find("takes one of: false, true"), std::string::npos) << json::dump(refused);
    auto undeclared = call(port, "flag_set", {{"tenant", "shop"}, {"box", "switches"}, {"flag", "paused"}, {"value", "true"},
                                              {"reason", "testing"}});
    EXPECT_NE(json::dump(undeclared).find("declares no flag paused"), std::string::npos) << json::dump(undeclared);
    auto no_reason = call(port, "flag_set", {{"tenant", "shop"}, {"box", "switches"}, {"flag", "frozen"}, {"value", "true"}});
    EXPECT_TRUE(no_reason.as_object().contains("error"));

    auto set = call(port, "flag_set", {{"tenant", "shop"}, {"box", "switches"}, {"flag", "frozen"}, {"value", "true"},
                                       {"reason", "inventory count tonight"}});
    ASSERT_FALSE(set.as_object().contains("error")) << json::dump(set);
    EXPECT_EQ(set.at("before"), "false");
    EXPECT_EQ(fetch(port, bhttp::verb::get, "/frozen").body, "true");  // the next call, same version

    auto list = call(port, "flag_list", {{"tenant", "shop"}});
    const auto& f = list.at("boxes").at(0).at("flags").at(0);
    EXPECT_EQ(f.at("value"), "true");
    EXPECT_EQ(f.at("default"), "false");
    EXPECT_EQ(f.at("set_why"), "inventory count tonight");

    // The change log keeps the flip, before and after.
    auto changes = call(port, "changes", {{"kind", "flag"}});
    EXPECT_NE(json::dump(changes).find("switches"), std::string::npos) << json::dump(changes);
    // The audit log names it with the reason.
    auto audit = base::read_file(root / "storage" / "logs" / "audit.log");
    ASSERT_TRUE(audit);
    EXPECT_NE(audit->find("\"flag.set\""), std::string::npos);
    EXPECT_NE(audit->find("inventory count tonight"), std::string::npos);
    // The access log records what the box read, for a replay.
    auto access = base::read_file(root / "storage" / "logs" / "access.log");
    ASSERT_TRUE(access);
    EXPECT_NE(access->find("\"flags\":{\"frozen\":\"true\"}"), std::string::npos);
  }
  // A restart keeps the value.
  host::Host again(cfg);
  auto port = again.start();
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/frozen").body, "true");
}

// whats_new serves host-src/CHANGES.md as built in: entries by day, filtered by day and word.
TEST_F(HostTest, WhatsNewSaysWhatThisBuildBrings) {
  auto all = tool("whats_new", {});
  const auto& r = all.at("result").at("structuredContent");
  ASSERT_FALSE(r.at("entries").as_array().empty());
  EXPECT_TRUE(r.as_object().contains("build"));
  std::string first_date(r.at("entries").at(0).at("date").as_string());
  auto since = tool("whats_new", {{"since", "2026-10-31"}}).at("result").at("structuredContent");
  for (const auto& e : since.at("entries").as_array()) EXPECT_GE(std::string(e.at("date").as_string()), "2026-10-31");
  auto about = tool("whats_new", {{"about", "route_try"}}).at("result").at("structuredContent");
  for (const auto& e : about.at("entries").as_array()) {
    EXPECT_NE(std::string(e.at("text").as_string()).find("route_try"), std::string::npos);
  }
  EXPECT_EQ(first_date.size(), 10u);  // YYYY-MM-DD
}

// Reviewing changes (design/machine/005.FirstVersion.md § 9.1): an upload says what widens
// against the live version, version_diff compares stored versions, and version_health sees a
// version that broke a route, with the rollback, after it went live.
TEST_F(HostTest, ChangesAreReviewedAndWatched) {
  auto wat = [](int status) {
    return std::string(R"w((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "text/plain")
  (func (export "goober_route_ping") (result i32)
    (drop (call $respond (i32.const )w") + std::to_string(status) + R"w() (i32.const 0) (i32.const 10) (i32.const 0) (i32.const 2)))
    (i32.const 0))
  (func (export "goober_route_admin") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 10) (i32.const 0) (i32.const 2)))
    (i32.const 0)))
)w";
  };
  auto manifest = [](bool open) {
    bj::object admin = open ? bj::object{{"name", "admin"}, {"public", true}}
                            : bj::object{{"name", "admin"}, {"requirements", bj::array{bj::array{"admin"}}}};
    return bj::object{{"box", "pinger"},
                      {"domains", bj::array{"*"}},
                      {"routes", bj::object{{"http", bj::object{
                          {"/ping", bj::object{{"get", bj::object{{"name", "ping"}, {"public", true}}}}},
                          {"/admin", bj::object{{"get", admin}}}}}}}};
  };
  auto result = [&](const char* name, bj::object args) { return tool(name, std::move(args)).at("result"); };

  // A new box: its routes are new reach.
  auto first = result("version_upload", {{"manifest", manifest(false)}, {"wat", wat(200)}});
  ASSERT_EQ(first.at("isError"), false) << json::dump(first);
  const auto& f = first.at("structuredContent");
  EXPECT_EQ(f.at("review").at("new_box"), true);
  EXPECT_NE(json::dump(f.at("widens")).find("new route GET /ping (public)"), std::string::npos) << json::dump(f.at("widens"));
  std::string v1(f.at("version").as_string());
  for (int i = 0; i < 25; ++i) ASSERT_EQ(fetch(port, bhttp::verb::get, "/ping").status, 200);

  // A dry run of a wider version says so, and changes nothing.
  auto dry = result("version_upload", {{"manifest", manifest(true)}, {"wat", wat(500)}, {"dry_run", true}});
  ASSERT_EQ(dry.at("isError"), false) << json::dump(dry);
  const auto& d = dry.at("structuredContent");
  EXPECT_EQ(d.at("stored"), false);
  EXPECT_EQ(json::dump(d.at("widens")), R"(["route GET /admin is now public"])");
  EXPECT_EQ(d.at("review").at("from").as_string(), v1);

  // Stored dark, then compared with version_diff's defaults (the live one against the newest).
  auto dark = result("version_upload", {{"manifest", manifest(true)}, {"wat", wat(500)}, {"activate", false}});
  ASSERT_EQ(dark.at("isError"), false) << json::dump(dark);
  std::string v2(dark.at("structuredContent").at("version").as_string());
  EXPECT_EQ(dark.at("structuredContent").at("review").at("to").as_string(), v2);
  auto diff = result("version_diff", {{"box", "pinger"}});
  ASSERT_EQ(diff.at("isError"), false) << json::dump(diff);
  EXPECT_EQ(diff.at("structuredContent").at("from").as_string(), v1);
  EXPECT_EQ(diff.at("structuredContent").at("to").as_string(), v2);
  EXPECT_NE(json::dump(diff.at("structuredContent").at("changes")).find("the module changed"), std::string::npos);

  // Live: /ping now fails, and version_health says so, with the rollback.
  ASSERT_EQ(result("version_promote", {{"box", "pinger"}, {"version", v2}}).at("isError"), false);
  for (int i = 0; i < 25; ++i) EXPECT_GE(fetch(port, bhttp::verb::get, "/ping").status, 500);
  auto health = result("version_health", {{"box", "pinger"}, {"minutes", 10}});
  ASSERT_EQ(health.at("isError"), false) << json::dump(health);
  const auto& hc = health.at("structuredContent");
  EXPECT_EQ(hc.at("previous").as_string(), v1);
  std::string findings = json::dump(hc.at("findings"));
  EXPECT_NE(findings.find("route ping: failures rose from 0.0% to 100.0%"), std::string::npos) << json::dump(hc);
  EXPECT_NE(findings.find("version_promote {box: \\\"pinger\\\", version: \\\"" + v1 + "\\\"} rolls back"), std::string::npos)
      << findings;
  // It never rolls back by itself.
  EXPECT_GE(fetch(port, bhttp::verb::get, "/ping").status, 500);
}
