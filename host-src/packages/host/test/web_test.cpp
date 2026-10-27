// The polite robot, crawls, and the web service boxes call (design/machine/017.Crawl.md),
// against a small website run by the test.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <mutex>
#include <thread>

#include "../src/web_fetch.h"
#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/server_http/server.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

struct Resource {
  int status = 200;
  std::string type = "text/html; charset=utf-8";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

// A ranch's website: a home page linking to products, a page robots.txt keeps us out of, a
// redirect, a sitemap, an image, and a Latin-1 page.
class Site {
 public:
  Site() {
    server::http::ServerOptions o;
    o.port = 0;
    o.threads = 1;
    server_ = std::make_unique<server::http::Server>(o, [this](server::http::Request req, server::http::Responder respond) {
      server::http::Response res;
      {
        std::lock_guard lock(mu_);
        hits_.push_back({req.path, std::chrono::steady_clock::now(), req.header("user-agent"), req.header("if-none-match")});
        auto it = pages_.find(req.path);
        if (it == pages_.end()) {
          res.status = 404;
          res.body = "nope";
        } else {
          res.status = it->second.status;
          res.headers.emplace_back("content-type", it->second.type);
          for (const auto& h : it->second.headers) res.headers.push_back(h);
          res.body = it->second.body;
          if (req.header("if-none-match") == "\"v1\"" && req.path == "/") {
            res.status = 304;
            res.body.clear();
          }
        }
      }
      respond(std::move(res));
    });
    server_->start();
    std::string base = "http://127.0.0.1:" + std::to_string(port());
    pages_["/robots.txt"] = {200, "text/plain", "User-agent: *\nDisallow: /private\n\nSitemap: " + base + "/sitemap.xml\n", {}};
    pages_["/"] = {200, "text/html", R"(<html><head><title>Hill Ranch</title>
<meta name="generator" content="WooCommerce 8"></head><body><h1>Hill Ranch</h1>
<a href="/beef">Beef</a> <a href="pork">Pork</a> <a href="/private/ledger">Books</a>
<a href="https://elsewhere.example/">Friends</a> <a href="/old" rel="nofollow">Old</a></body></html>)",
                   {{"etag", "\"v1\""}}};
    pages_["/beef"] = {200, "text/html", R"(<html><body>
<script type="application/ld+json">{"@context":"https://schema.org","@type":"Product","name":"Quarter beef",
"offers":{"@type":"Offer","price":"950","priceCurrency":"USD"}}</script>
<div class="cut"><b class="n">Ribeye</b> <i class="p">$24/lb</i></div>
<div class="cut"><b class="n">Brisket</b> <i class="p">$11/lb</i></div></body></html>)", {}};
    pages_["/pork"] = {301, "text/html", "", {{"location", "/pork-cuts"}}};
    pages_["/pork-cuts"] = {200, "text/html", R"(<div class="cut"><b class="n">Bacon</b><i class="p">$12/lb</i></div>)", {}};
    pages_["/private/ledger"] = {200, "text/html", "secret", {}};
    pages_["/old"] = {200, "text/html", "old", {}};
    pages_["/photo.png"] = {200, "image/png", std::string("\x89PNG\r\n", 6), {}};
    pages_["/latin"] = {200, "text/html; charset=iso-8859-1", "<p>Jalape\xF1o \x93sausage\x94</p>", {}};
    pages_["/sitemap.xml"] = {200, "application/xml",
                              "<urlset><url><loc>" + base + "/lamb</loc></url><url><loc>https://elsewhere.example/x</loc>"
                              "</url></urlset>", {}};
    pages_["/lamb"] = {200, "text/html", "<p>Lamb shares</p>", {}};
  }
  uint16_t port() const { return server_->port(); }
  std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port()) + path; }
  struct Hit {
    std::string path;
    std::chrono::steady_clock::time_point at;
    std::string user_agent, if_none_match;
  };
  std::vector<Hit> hits() {
    std::lock_guard lock(mu_);
    return hits_;
  }
  int count(const std::string& path) {
    int n = 0;
    for (const auto& h : hits()) n += h.path == path;
    return n;
  }

 private:
  std::mutex mu_;
  std::map<std::string, Resource> pages_;
  std::vector<Hit> hits_;
  std::unique_ptr<server::http::Server> server_;
};

struct Reply {
  int status = 0;
  std::string body;
};

Reply post(uint16_t port, const std::string& target, const std::string& body = "", const std::string& auth = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(bhttp::verb::post, target, 11);
  req.set(bhttp::field::host, "localhost");
  if (!auth.empty()) req.set(bhttp::field::authorization, auth);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  return {int(res.result_int()), res.body()};
}

// A box route that fetches one URL through its web service and answers with the result.
std::string box_wat(const std::string& request_json) {
  std::string escaped;
  for (char c : request_json) escaped += c == '"' ? std::string("\\22") : std::string(1, c);
  return R"((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "farms")
  (data (i32.const 24) "fetch")
  (data (i32.const 64) ")" + escaped + R"(")
  (func (export "goober_route_scout") (result i32)
    (local $n i32)
    (local.set $n (call $call (i32.const 16) (i32.const 5) (i32.const 24) (i32.const 5) (i32.const 64) (i32.const )" +
         std::to_string(request_json.size()) + R"()))
    (drop (call $result (i32.const 8192) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 8192) (local.get $n)))
    (i32.const 0))
))";
}

}  // namespace

class WebTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "web";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "operations": {"token": "test-token-0123456789"},
        "threads": {"network": 1, "boxes": 2}, "limits": {"timeout-ms": {"default": 5000, "max": 10000}},
        "web": {"contact": "https://ops.example/robot", "min-delay-ms": 300}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = post(port, "/goober-operations-mcp", json::dump(req), "Bearer test-token-0123456789");
    std::string err;
    auto doc = json::parse(r.body, &err);
    const auto& result = doc->at("result");
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::object{{"error", text}};
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  }

  Site site;
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST(PublicAddress, RefusesEverythingButThePublicInternet) {
  auto v4 = [](const char* a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    inet_pton(AF_INET, a, &s.sin_addr);
    return host::public_address(reinterpret_cast<sockaddr*>(&s));
  };
  auto v6 = [](const char* a) {
    sockaddr_in6 s{};
    s.sin6_family = AF_INET6;
    inet_pton(AF_INET6, a, &s.sin6_addr);
    return host::public_address(reinterpret_cast<sockaddr*>(&s));
  };
  for (const char* bad : {"127.0.0.1", "10.1.2.3", "172.16.0.1", "192.168.1.1", "169.254.169.254", "100.64.0.1",
                          "0.0.0.0", "224.0.0.1", "255.255.255.255", "198.18.0.1", "192.0.2.1"}) {
    EXPECT_FALSE(v4(bad)) << bad;
  }
  EXPECT_TRUE(v4("93.184.216.34"));
  EXPECT_TRUE(v4("8.8.8.8"));
  for (const char* bad : {"::1", "::", "fe80::1", "fc00::1", "fd12::1", "ff02::1", "::ffff:127.0.0.1", "::ffff:10.0.0.1",
                          "2001:db8::1", "64:ff9b::a00:1", "2002:a00:1::1"}) {
    EXPECT_FALSE(v6(bad)) << bad;
  }
  EXPECT_TRUE(v6("2606:4700::1111"));
  EXPECT_TRUE(v6("::ffff:8.8.8.8"));
}

TEST_F(WebTest, OperatorsReadPagesPolitelyAsViews) {
  auto page = tool("web_fetch", {{"url", site.url("/beef")},
                                 {"views", bj::array{"meta", "jsonld", "text"}},
                                 {"extract", bj::object{{"items", ".cut"}, {"fields", bj::object{{"name", ".n"}, {"price", ".p"}}}}}});
  ASSERT_FALSE(page.as_object().contains("error")) << json::dump(page);
  EXPECT_EQ(page.at("status"), 200);
  EXPECT_EQ(page.at("jsonld").at(0).at("offers").at("price"), "950");
  EXPECT_EQ(json::dump(page.at("extract")),
            R"([{"name":"Ribeye","price":"$24/lb"},{"name":"Brisket","price":"$11/lb"}])");
  EXPECT_NE(std::string(page.at("text").as_string()).find("Ribeye $24/lb"), std::string::npos);
  // It asked robots.txt first, said who it is, and the second read comes from the cache.
  ASSERT_GE(site.hits().size(), 2u);
  EXPECT_EQ(site.hits()[0].path, "/robots.txt");
  EXPECT_NE(site.hits()[1].user_agent.find("goober/"), std::string::npos);
  EXPECT_NE(site.hits()[1].user_agent.find("https://ops.example/robot"), std::string::npos);
  auto again = tool("web_fetch", {{"url", site.url("/beef")}, {"views", bj::array{"html"}}});
  EXPECT_EQ(again.at("from_cache"), true);
  EXPECT_EQ(site.count("/beef"), 1);

  // robots.txt is obeyed; images aren't pages; redirects are followed and reported.
  auto denied = tool("web_fetch", {{"url", site.url("/private/ledger")}});
  EXPECT_NE(json::dump(denied).find("robots.txt"), std::string::npos) << json::dump(denied);
  EXPECT_EQ(site.count("/private/ledger"), 0);
  auto image = tool("web_fetch", {{"url", site.url("/photo.png")}});
  EXPECT_NE(json::dump(image).find("not a page"), std::string::npos) << json::dump(image);
  auto moved = tool("web_fetch", {{"url", site.url("/pork")}, {"views", bj::array{"text"}}});
  EXPECT_EQ(std::string(moved.at("url").as_string()), site.url("/pork-cuts"));
  // Latin-1 arrives as UTF-8.
  auto latin = tool("web_fetch", {{"url", site.url("/latin")}, {"views", bj::array{"text"}}});
  EXPECT_EQ(latin.at("text"), "Jalape\xC3\xB1o \xE2\x80\x9Csausage\xE2\x80\x9D");
  // A refetch is conditional, and a 304 serves the copy we have.
  tool("web_fetch", {{"url", site.url("/")}});
  auto fresh = tool("web_fetch", {{"url", site.url("/")}, {"max_age_ms", 0}, {"views", bj::array{"meta"}}});
  EXPECT_EQ(fresh.at("meta").at("title"), "Hill Ranch");
  EXPECT_EQ(fresh.at("meta").at("platform"), "woocommerce");
  EXPECT_EQ(site.hits().back().if_none_match, "\"v1\"");

  // One site is asked at most once per min-delay-ms.
  auto hits = site.hits();
  for (size_t i = 1; i < hits.size(); ++i) {
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(hits[i].at - hits[i - 1].at).count(), 280)
        << hits[i - 1].path << " then " << hits[i].path;
  }
  // The egress log has every fetch.
  auto log = base::read_file(root / "storage/logs/egress.log");
  ASSERT_TRUE(log);
  EXPECT_NE(log->find(R"("service":"web")"), std::string::npos);

  // Opted-out hosts are never fetched.
  tool("web_optout", {{"host", "127.0.0.1"}});
  auto refused = tool("web_fetch", {{"url", site.url("/lamb")}});
  EXPECT_NE(json::dump(refused).find("asked not to be crawled"), std::string::npos) << json::dump(refused);
}

TEST_F(WebTest, CrawlsFollowLinksWithinScopeAndRecordWhatTheyFind) {
  auto started = tool("crawl_start", {{"name", "hill"},
                                      {"seeds", bj::array{site.url("/")}},
                                      {"sitemaps", true},
                                      {"max_depth", 2},
                                      {"extract", bj::object{{"items", ".cut"}, {"fields", bj::object{{"name", ".n"}}}}}});
  ASSERT_FALSE(started.as_object().contains("error")) << json::dump(started);
  int64_t id = started.at("crawl").as_int64();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::string state;
  while (std::chrono::steady_clock::now() < deadline) {
    auto list = tool("crawl_list", {});
    state = std::string(list.at(0).at("state").as_string());
    if (state == "done") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_EQ(state, "done") << json::dump(tool("crawl_pages", {{"crawl", id}}));
  auto pages = json::dump(tool("crawl_pages", {{"crawl", id}}));
  for (const char* want : {"/beef", "/pork", "/lamb", "/private/ledger"}) {
    EXPECT_NE(pages.find(site.url(want)), std::string::npos) << want << " in " << pages;
  }
  EXPECT_EQ(pages.find("elsewhere.example"), std::string::npos);  // out of scope
  EXPECT_EQ(pages.find(site.url("/old")), std::string::npos);     // rel=nofollow
  EXPECT_EQ(site.count("/private/ledger"), 0);                    // robots.txt
  auto errors = json::dump(tool("crawl_pages", {{"crawl", id}, {"state", "error"}}));
  EXPECT_NE(errors.find("robots.txt"), std::string::npos) << errors;

  auto products = tool("crawl_records", {{"crawl", id}, {"kind", "Product"}});
  ASSERT_EQ(products.as_array().size(), 1u) << json::dump(products);
  EXPECT_EQ(products.at(0).at("data").at("name"), "Quarter beef");
  auto cuts = json::dump(tool("crawl_records", {{"crawl", id}, {"kind", "extract"}}));
  for (const char* cut : {"Ribeye", "Brisket", "Bacon"}) EXPECT_NE(cuts.find(cut), std::string::npos) << cuts;

  // A rerun starts from the seeds again.
  auto rerun = tool("crawl_rerun", {{"crawl", id}});
  EXPECT_EQ(rerun.at("state"), "running");
  tool("crawl_stop", {{"crawl", id}});
  EXPECT_EQ(tool("crawl_list", {}).at(0).at("state"), "stopped");
}

TEST_F(WebTest, BoxesFetchThroughTheirWebService) {
  std::string err;
  auto manifest = [&](const std::string& box, const std::string& route) {
    return R"({"box":")" + box + R"(","domains":["*"],"behavior":{"timeout-ms":8000},
      "external":{"farms":{"web":{"hosts":["127.0.0.1"]},"methods":{"fetch":{"budget":{"calls":2}}}}},
      "routes":{"http":{"/)" + route + R"(":{"post":{"name":"scout","public":true}}}}})";
  };
  std::string wat = box_wat(R"({"url":")" + site.url("/beef") + R"(","views":["jsonld"],"select":".n"})");
  ASSERT_TRUE(host->boxes().upload(manifest("scout", "scout"), wat, &err)) << err;
  auto r = post(port, "/scout");
  ASSERT_EQ(r.status, 200) << r.body;
  std::string e;
  auto doc = json::parse(r.body, &e);
  ASSERT_TRUE(doc) << r.body;
  EXPECT_EQ(doc->at("status"), 200);
  EXPECT_EQ(doc->at("jsonld").at(0).at("name"), "Quarter beef");
  EXPECT_EQ(doc->at("select").at(1).at("text"), "Brisket");

  // A host the manifest doesn't list is refused before anything goes out.
  std::string other = box_wat(R"({"url":"http://localhost:)" + std::to_string(site.port()) + R"(/beef"})");
  ASSERT_TRUE(host->boxes().upload(manifest("scout", "scout"), other, &err)) << err;
  r = post(port, "/scout");
  EXPECT_NE(r.body.find("web.hosts doesn't include localhost"), std::string::npos) << r.body;

  // Manifests say which hosts, and nothing else.
  for (const char* bad : {R"({"hosts":[]})", R"({"hosts":["not a host"]})", R"({"hosts":["*"],"x":1})", R"([])"}) {
    std::string m = R"({"box":"bad","domains":["*"],"external":{"w":{"web":)" + std::string(bad) + R"(}},
      "routes":{"http":{"/b":{"post":{"name":"bad_route","public":true}}}}})";
    EXPECT_FALSE(host->boxes().upload(m, "(module (memory (export \"memory\") 1))", &err)) << bad;
  }
}
