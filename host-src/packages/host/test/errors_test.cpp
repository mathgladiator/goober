// Errors as visitors see them and as boxes cause them (design/machine/007.Content.md § 13;
// design/machine/028.Panel.md § 4.2, § 4.3): the site's error pages, a box's empty 4xx, the
// sitemap's icicle without them, error_report, and box_failures.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/encoding.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kToken = "errors-token-0123456789";

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
  req.set(bhttp::field::host, "example.com");
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

const char* kWat = R"wat((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22error\22:\22no such place\22}")
  (func (export "goober_route_gone") (result i32)
    (drop (call $respond (i32.const 404) (i32.const 0) (i32.const 16) (i32.const 0) (i32.const 0))) (i32.const 0))
  (func (export "goober_route_said") (result i32)
    (drop (call $respond (i32.const 404) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 25))) (i32.const 0))
  (func (export "goober_route_down") (result i32)
    (drop (call $respond (i32.const 503) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 25))) (i32.const 0))
  (func (export "goober_route_boom") (result i32) unreachable)
  (func (export "goober_route_members") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 25))) (i32.const 0))
  (func (export "goober_route_fine") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 25))) (i32.const 0))
))wat";

class ErrorsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "errors";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"example.com": {"tenant": "example"}},
        "operations": {"token": ")" + std::string(kToken) + R"("},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    ASSERT_TRUE(host->boxes().upload(R"({"box":"places","domains":["*"],"routes":{"http":{
        "/places/gone":{"get":{"name":"gone","public":true}},
        "/places/said":{"get":{"name":"said","public":true}},
        "/places/down":{"get":{"name":"down","public":true}},
        "/places/boom":{"get":{"name":"boom","public":true}},
        "/places/fine":{"get":{"name":"fine","public":true}},
        "/members":{"get":{"name":"members","requirements":[["member"]]}}}}})",
                                     kWat, &err))
        << err;
  }
  void TearDown() override { host.reset(); }

  bj::value op(const std::string& tool, bj::object args = {}) {
    bj::object req{{"jsonrpc", "2.0"},
                   {"id", 1},
                   {"method", "tools/call"},
                   {"params", bj::object{{"name", tool}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", std::string("Bearer ") + kToken}, {"content-type", "application/json"}},
                   json::dump(req));
    std::string err;
    auto v = json::parse(r.body, &err);
    EXPECT_TRUE(v) << r.body;
    const auto& res = v->at("result");
    EXPECT_FALSE(res.at("isError").as_bool()) << r.body;
    if (res.as_object().contains("structuredContent")) return res.at("structuredContent");
    return *json::parse(std::string(res.at("content").at(0).at("text").as_string()), &err);
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

}  // namespace

// An error page is a template in the site's layout, with its partials, rendered for nobody.
TEST_F(ErrorsTest, ErrorPagesWearTheSiteLayoutAndArentPagesOfTheirOwn) {
  std::string err;
  ASSERT_TRUE(host->content().put("example", "_partials/footer.html", "<footer>Example Co</footer>", &err));
  ASSERT_TRUE(host->content().put("example", "index.md", "# Home", &err));
  ASSERT_TRUE(host->content().put("example", "error-message/404.html",
                                  "---\nlayout: base\n---\n<h1>{{status}} {{reason}}</h1>\n<p>Lost? <a href=\"/\">Home</a></p>"
                                  "\n<!--#include footer-->\n<small>{{request_id}}</small>",
                                  &err));
  auto r = fetch(port, bhttp::verb::get, "/no/such/page", {{"accept", "text/html"}});
  EXPECT_EQ(r.status, 404);
  EXPECT_NE(r.body.find("<h1>404 Not Found</h1>"), std::string::npos) << r.body;
  EXPECT_NE(r.body.find("<footer>Example Co</footer>"), std::string::npos) << r.body;
  EXPECT_NE(r.body.find("<html"), std::string::npos) << r.body;  // the base layout around it
  EXPECT_NE(r.body.find(r.headers["goober-request-id"]), std::string::npos);
  EXPECT_EQ(r.headers["cache-control"], "no-store");
  // An API caller gets the host's JSON instead.
  auto api = fetch(port, bhttp::verb::get, "/no/such/page", {{"accept", "application/json"}});
  EXPECT_EQ(api.status, 404);
  EXPECT_NE(api.body.find(R"("error":"Not Found")"), std::string::npos) << api.body;
  // The error page is no page: not at its own URL, not in the sitemap.
  auto direct = fetch(port, bhttp::verb::get, "/error-message/404", {{"accept", "text/html"}});
  EXPECT_EQ(direct.status, 404);
  auto sitemap = fetch(port, bhttp::verb::get, "/sitemap.xml");
  EXPECT_EQ(sitemap.body.find("error-message"), std::string::npos) << sitemap.body;
  // page_put won't write one (it would be a page); content_put does.
  auto refused = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                       {{"authorization", std::string("Bearer ") + kToken}, {"content-type", "application/json"}},
                       R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"page_put","arguments":)"
                       R"({"tenant":"example","path":"/error-message/403","format":"document","body":"<p>no</p>"}}})");
  EXPECT_NE(refused.body.find("content_put"), std::string::npos) << refused.body;
}

// A box's 4xx with nothing in it gets the site's error page; one with a body keeps its own.
TEST_F(ErrorsTest, ABoxsEmpty404GetsTheSitesPage) {
  std::string err;
  ASSERT_TRUE(host->content().put("example", "error-message/404.html", "<p>The site's {{status}}</p>", &err));
  auto gone = fetch(port, bhttp::verb::get, "/places/gone", {{"accept", "text/html"}});
  EXPECT_EQ(gone.status, 404);
  EXPECT_EQ(gone.body, "<p>The site's 404</p>");
  EXPECT_EQ(gone.headers["content-type"], "text/html; charset=utf-8");
  auto gone_api = fetch(port, bhttp::verb::get, "/places/gone", {{"accept", "application/json"}});
  EXPECT_EQ(gone_api.status, 404);
  EXPECT_NE(gone_api.body.find(R"("status":404)"), std::string::npos) << gone_api.body;
  auto said = fetch(port, bhttp::verb::get, "/places/said", {{"accept", "text/html"}});
  EXPECT_EQ(said.status, 404);
  EXPECT_EQ(said.body, R"({"error":"no such place"})");
}

// The icicle holds the site's traffic; errors have their own view, and failures theirs.
TEST_F(ErrorsTest, ErrorsStayOutOfTheTreeAndShowInTheirViews) {
  for (int i = 0; i < 3; ++i) fetch(port, bhttp::verb::get, "/wp-login-" + std::to_string(i) + ".php");
  fetch(port, bhttp::verb::get, "/members", {{"accept", "application/json"}});  // 401: the gate
  fetch(port, bhttp::verb::get, "/places/boom", {{"referer", "https://elsewhere.example/"}});
  fetch(port, bhttp::verb::get, "/places/down");
  fetch(port, bhttp::verb::get, "/places/gone", {{"referer", "http://example.com/"}});
  for (int i = 0; i < 4; ++i) fetch(port, bhttp::verb::get, "/places/fine");

  auto tree = json::dump(op("uri_tree", {{"tenant", "example"}}));
  EXPECT_EQ(tree.find("not found"), std::string::npos) << tree;
  EXPECT_EQ(tree.find("refused"), std::string::npos) << tree;
  EXPECT_EQ(tree.find("wp-login"), std::string::npos) << tree;
  EXPECT_NE(tree.find("\"boom\""), std::string::npos) << tree;  // a box route's failures stay on it
  EXPECT_NE(tree.find("\"gone\""), std::string::npos) << tree;  // and its own 404s

  auto e = op("error_report", {{"window", "1h"}, {"tenant", "example"}});
  const auto& totals = e.at("totals").as_object();
  EXPECT_EQ(totals.at("host").as_int64(), 4) << json::dump(e);  // three scans and the gate
  EXPECT_EQ(totals.at("box").as_int64(), 1) << json::dump(e);
  EXPECT_EQ(totals.at("box_failed").as_int64(), 2) << json::dump(e);
  std::string places = json::dump(e.at("places"));
  EXPECT_NE(places.find(R"j("place":"(not found)")j"), std::string::npos) << places;
  EXPECT_EQ(places.find("wp-login"), std::string::npos) << places;
  EXPECT_EQ(e.at("referred").at("other").as_int64(), 1);
  EXPECT_EQ(e.at("referred").at("same").as_int64(), 1);

  auto f = op("box_failures", {{"window", "1h"}, {"box", "places"}});
  const auto& classes = f.at("totals").at("classes").as_object();
  EXPECT_EQ(classes.at("trap").as_int64(), 1) << json::dump(f);
  EXPECT_EQ(classes.at("bad_status").as_int64(), 1) << json::dump(f);
  EXPECT_EQ(f.at("totals").at("failures").as_int64(), 2);
  EXPECT_EQ(f.at("totals").at("calls").as_int64(), 7);  // the box ran 7 times; the gate stopped one
  bool trap_said = false;
  for (const auto& l : f.at("latest").as_array()) {
    trap_said |= l.at("class") == "trap" && l.at("route") == "boom" && l.at("kind") == "get";
  }
  EXPECT_TRUE(trap_said) << json::dump(f);
}
