// Route caching (design/machine/037.Caching.md § 3 to § 5): a box's GET answers kept while
// what they read is unchanged, never shared across people, checked by sampling, and answered
// with a 304 without waking the box.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <fstream>
#include <regex>
#include <thread>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/manifest/manifest.h"

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
  req.set(bhttp::field::host, "shop.example.com");
  for (const auto& [k, v] : headers) req.set(k, v);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response_parser<bhttp::string_body> parser;
  parser.skip(verb == bhttp::verb::head);  // a HEAD's answer has headers only
  bhttp::read(stream, buf, parser);
  auto res = parser.release();
  Reply r{int(res.result_int()), res.body(), {}};
  for (const auto& f : res) r.headers[base::to_lower(std::string(f.name_string()))] = std::string(f.value());
  return r;
}

const char* kSchema = R"({"schema": "menu1", "tables": {"items": {"columns": {
    "id": {"type": "id", "primary-key": true}, "name": {"type": "string"}}}}})";

const char* kWat = R"wat((module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_log" (func $log (param i32 i32 i32) (result i32)))
  (memory (export "memory") 4)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "s")
  (data (i32.const 64) "q")
  (data (i32.const 96) "SELECT json_group_array(name) AS names FROM items")
  (data (i32.const 176) "INSERT INTO items (name) VALUES ('x')")
  (data (i32.const 240) "[]")
  (data (i32.const 272) "ran menu")
  (data (i32.const 304) "ran wake")
  (data (i32.const 336) "ran mine")
  (data (i32.const 368) "ran clock")
  (data (i32.const 400) "ran big")
  (data (i32.const 432) "{\"pad\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}")
  ;; Answers the last SQL result as the body.
  (func $answer_sql (param $n i32)
    (drop (call $result (i32.const 8192) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 8192) (local.get $n))))
  ;; Answers the request document itself as the body.
  (func $answer_doc
    (local $n i32)
    (local.set $n (call $read (i32.const 8192) (call $size)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 8192) (local.get $n))))
  (func (export "goober_route_menu") (result i32)
    (drop (call $log (i32.const 1) (i32.const 272) (i32.const 8)))
    (call $answer_sql (call $sql (i32.const 32) (i32.const 1) (i32.const 96) (i32.const 49) (i32.const 240) (i32.const 2)))
    (i32.const 0))
  (func (export "goober_route_menu_wake") (result i32)
    (drop (call $log (i32.const 1) (i32.const 304) (i32.const 8)))
    (call $answer_sql (call $sql (i32.const 32) (i32.const 1) (i32.const 96) (i32.const 49) (i32.const 240) (i32.const 2)))
    (i32.const 0))
  (func (export "goober_route_mine") (result i32)
    (drop (call $log (i32.const 1) (i32.const 336) (i32.const 8)))
    (call $answer_doc)
    (i32.const 0))
  (func (export "goober_route_clock") (result i32)
    (drop (call $log (i32.const 1) (i32.const 368) (i32.const 9)))
    (call $answer_doc)
    (i32.const 0))
  (func (export "goober_route_big") (result i32)
    (drop (call $log (i32.const 1) (i32.const 400) (i32.const 7)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 432) (i32.const 1510)))
    (i32.const 0))
  (func (export "goober_route_add") (result i32)
    (drop (call $sql (i32.const 32) (i32.const 1) (i32.const 176) (i32.const 37) (i32.const 240) (i32.const 2)))
    (drop (call $respond (i32.const 201) (i32.const 0) (i32.const 16) (i32.const 240) (i32.const 2)))
    (i32.const 0))
  (func (export "goober_route_add_quiet") (result i32)
    (drop (call $sql (i32.const 64) (i32.const 1) (i32.const 176) (i32.const 37) (i32.const 240) (i32.const 2)))
    (drop (call $respond (i32.const 201) (i32.const 0) (i32.const 16) (i32.const 240) (i32.const 2)))
    (i32.const 0))
))wat";

const char* kManifest = R"({"box": "menu", "domains": ["*"],
  "data": {"sqlite3": {
    "s": {"schema": "menu1", "database": "menu", "pattern": "sql", "publish": "menu",
          "tables": {"items": {"read": true, "insert": true}}},
    "q": {"schema": "menu1", "database": "menu", "pattern": "sql",
          "tables": {"items": {"read": true, "insert": true}}}}},
  "routes": {"http": {
    "/menu": {"get": {"name": "menu", "public": true, "cache": {"by": "data", "check-percent": 0}}},
    "/menu-wake": {"get": {"name": "menu_wake", "public": true,
                           "cache": {"by": "wake", "wake": ["menu"], "fresh-ms": 1000, "check-percent": 0}}},
    "/mine": {"get": {"name": "mine", "requirements": [], "cache": {"check-percent": 0, "browser-ms": 30000}}},
    "/clock": {"get": {"name": "clock", "public": true, "cache": {"check-percent": 100}}},
    "/big": {"get": {"name": "big", "public": true, "cache": {"check-percent": 0}}},
    "/items": {"post": {"name": "add", "public": true}},
    "/quiet": {"post": {"name": "add_quiet", "public": true}}}}})";

}  // namespace

class CacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "route_cache";
    fs::remove_all(root);
    std::string err;
    // A budget of 16 KiB: 4 KiB per tenant, so the 1.5 KiB answers of /big push each other out.
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"shop.example.com": {"tenant": "shop"}}, "threads": {"network": 1, "boxes": 2},
        "operations": {"token": "op-token-for-cache-tests-0123456789"}, "cache": {"bytes": 16384}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    auto put = tool("schema_put", {{"schema", kSchema}});
    ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
    ASSERT_TRUE(host->boxes().upload(kManifest, kWat, &err)) << err;
  }
  void TearDown() override { host.reset(); }

  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer op-token-for-cache-tests-0123456789"}, {"content-type", "application/json"}},
                   json::dump(req));
    std::string err;
    auto doc = json::parse(r.body, &err);
    const auto& result = doc->at("result").as_object();
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::object{{"error", text}};
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  }
  // How many times the box ran a route: its log lines.
  int ran(const std::string& what) {
    std::ifstream in(root / "storage/logs/boxes/menu.log");
    std::string line;
    int n = 0;
    while (std::getline(in, line)) n += line.find("\"ran " + what + "\"") != std::string::npos;
    return n;
  }
  bj::object stats(const std::string& route) {
    auto s = tool("cache_status", {{"tenant", "shop"}});
    for (const auto& r : s.at("routes").as_array()) {
      if (r.at("route").as_string() == route) return r.as_object();
    }
    ADD_FAILURE() << "no route " << route << " in " << json::dump(s);
    return {};
  }
  std::string sign_in(const std::string& email) {
    fetch(port, bhttp::verb::post, "/identity/email/start",
          {{"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://shop.example.com"}}, "email=" + email);
    host->drain_mail();
    std::string code;
    for (const auto& f : base::list_files(root / "storage/mail/outbox")) {
      std::smatch m;
      auto text = *base::read_file(root / "storage/mail/outbox" / f);
      if (text.find("To: " + email) != std::string::npos && std::regex_search(text, m, std::regex("is ([0-9]{6})"))) code = m[1];
    }
    auto v = fetch(port, bhttp::verb::post, "/identity/email/verify",
                   {{"content-type", "application/x-www-form-urlencoded"}, {"origin", "http://shop.example.com"}},
                   "email=" + email + "&code=" + code);
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(CacheTest, HitsSkipTheBoxAndAWriteMakesThemStale) {
  // The database exists before the first read (its first request creates it, a change that
  // makes any answer read before it stale).
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/quiet", {{"origin", "http://shop.example.com"}}).status, 201);
  auto first = fetch(port, bhttp::verb::get, "/menu");
  ASSERT_EQ(first.status, 200) << first.body;
  EXPECT_EQ(first.headers["cache-control"], "public, no-cache");
  ASSERT_FALSE(first.headers["etag"].empty());
  auto second = fetch(port, bhttp::verb::get, "/menu");
  EXPECT_EQ(second.body, first.body);
  EXPECT_EQ(ran("menu"), 1);  // the second came from the cache
  // A HEAD shares the GET's entry.
  EXPECT_EQ(fetch(port, bhttp::verb::head, "/menu").status, 200);
  EXPECT_EQ(ran("menu"), 1);
  // 304 without waking the box.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/menu", {{"if-none-match", first.headers["etag"]}}).status, 304);
  EXPECT_EQ(ran("menu"), 1);
  // A write to the database it read: stale, and the box runs again with the new row.
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/items", {{"origin", "http://shop.example.com"}}).status, 201);
  auto third = fetch(port, bhttp::verb::get, "/menu");
  EXPECT_NE(third.body, first.body);
  EXPECT_NE(third.body.find("\\\"x\\\",\\\"x\\\""), std::string::npos) << third.body;  // two rows now
  EXPECT_EQ(ran("menu"), 2);
  // An operator's db_exec is a write the host makes too.
  auto exec = tool("db_exec", {{"tenant", "shop"}, {"database", "menu"}, {"sql", "INSERT INTO items (name) VALUES ('y')"}});
  ASSERT_FALSE(exec.as_object().contains("error")) << json::dump(exec);
  EXPECT_NE(fetch(port, bhttp::verb::get, "/menu").body.find("y"), std::string::npos);
  EXPECT_EQ(ran("menu"), 3);
  // A write the host doesn't make (a shell on the file) is caught by SQLite's data_version.
  {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open((root / "storage/sqlite3/shop/databases/menu.db").c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "UPDATE items SET name = 'z' WHERE name = 'y'", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
  }
  EXPECT_NE(fetch(port, bhttp::verb::get, "/menu").body.find("z"), std::string::npos);
  EXPECT_EQ(ran("menu"), 4);
  auto s = stats("menu");
  EXPECT_EQ(s.at("hits").as_int64(), 3);
  EXPECT_EQ(s.at("not_modified").as_int64(), 1);
  EXPECT_EQ(s.at("stale").as_int64(), 3);
  // The access log says which.
  auto log = *base::read_file(root / "storage/logs/access.log");
  EXPECT_NE(log.find("\"cache\":\"hit\""), std::string::npos);
  EXPECT_NE(log.find("\"cache\":\"304\""), std::string::npos);
  // cache_clear empties it: the next request runs the box.
  EXPECT_EQ(tool("cache_clear", {{"tenant", "shop"}, {"route", "menu"}}).at("cleared"), 1);
  fetch(port, bhttp::verb::get, "/menu");
  EXPECT_EQ(ran("menu"), 5);
}

TEST_F(CacheTest, ByWakeGoesStaleOnlyWhenItsTopicMovesOrFreshMsPasses) {
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/items", {{"origin", "http://shop.example.com"}}).status, 201);
  fetch(port, bhttp::verb::get, "/menu-wake");
  EXPECT_EQ(ran("wake"), 1);
  // A write that publishes nothing: by wake, the entry stays (the declaration says so).
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/quiet", {{"origin", "http://shop.example.com"}}).status, 201);
  auto kept = fetch(port, bhttp::verb::get, "/menu-wake");
  EXPECT_NE(kept.body.find("[\\\"x\\\"]"), std::string::npos) << kept.body;  // still one row
  EXPECT_EQ(ran("wake"), 1);
  // A write that publishes "menu": stale.
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/items", {{"origin", "http://shop.example.com"}}).status, 201);
  EXPECT_NE(fetch(port, bhttp::verb::get, "/menu-wake").body.find("\\\"x\\\",\\\"x\\\",\\\"x\\\""), std::string::npos);
  EXPECT_EQ(ran("wake"), 2);
  // fresh-ms (1 s here) bounds an entry whatever happens.
  fetch(port, bhttp::verb::get, "/menu-wake");
  EXPECT_EQ(ran("wake"), 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  fetch(port, bhttp::verb::get, "/menu-wake");
  EXPECT_EQ(ran("wake"), 3);
}

TEST_F(CacheTest, OnePersonsEntryIsNeverAnothersAndPublicRoutesSeeNoOne) {
  auto ada = sign_in("ada@example.com");
  auto bo = sign_in("bo@example.com");
  ASSERT_FALSE(ada.empty());
  ASSERT_FALSE(bo.empty());
  auto a1 = fetch(port, bhttp::verb::get, "/mine", {{"cookie", ada}});
  ASSERT_EQ(a1.status, 200) << a1.body;
  auto b1 = fetch(port, bhttp::verb::get, "/mine", {{"cookie", bo}});
  EXPECT_NE(a1.body.find("ada@example.com"), std::string::npos);
  EXPECT_NE(b1.body.find("bo@example.com"), std::string::npos);
  EXPECT_EQ(b1.body.find("ada@example.com"), std::string::npos);
  auto a2 = fetch(port, bhttp::verb::get, "/mine", {{"cookie", ada}});
  EXPECT_EQ(a2.body, a1.body);
  EXPECT_EQ(ran("mine"), 2);  // one per person, then a hit
  EXPECT_EQ(a1.headers["cache-control"], "private, max-age=30");
  // Signed out: the gate answers before the cache.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/mine").status, 401);
  // A cached public route's box sees no user and no request headers but the declared ones.
  auto c = fetch(port, bhttp::verb::get, "/clock", {{"cookie", ada}, {"x-extra", "1"}});
  EXPECT_NE(c.body.find("\"user\":null"), std::string::npos) << c.body;
  EXPECT_EQ(c.body.find("x-extra"), std::string::npos) << c.body;
  EXPECT_EQ(c.body.find("client_key"), std::string::npos) << c.body;
  EXPECT_EQ(c.headers["cache-control"], "private, no-cache");  // a signed-in reader's copy is theirs
}

TEST_F(CacheTest, ChecksCatchAnAnswerThatDependsOnMore) {
  // /clock answers its request id, so every check finds a different answer.
  fetch(port, bhttp::verb::get, "/clock");
  fetch(port, bhttp::verb::get, "/clock");  // a hit, checked (100 %)
  for (int i = 0; i < 50 && stats("clock").at("checks").as_int64() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  auto s = stats("clock");
  EXPECT_EQ(s.at("checks").as_int64(), 1);
  EXPECT_EQ(s.at("mismatches").as_int64(), 1);
  EXPECT_EQ(ran("clock"), 2);
  auto log = *base::read_file(root / "storage/logs/boxes/menu.log");
  EXPECT_NE(log.find("cache check: route clock answered differently"), std::string::npos);
}

TEST_F(CacheTest, TheBudgetEvictsLeastRecentlyUsed) {
  for (int i = 0; i < 6; ++i) ASSERT_EQ(fetch(port, bhttp::verb::get, "/big?n=" + std::to_string(i)).status, 200);
  auto s = tool("cache_status", {{"tenant", "shop"}});
  EXPECT_LE(s.at("bytes").as_int64(), s.at("tenant_budget").as_int64());
  EXPECT_GE(stats("big").at("evictions").as_int64(), 3);
  // The newest is still there; the oldest went.
  fetch(port, bhttp::verb::get, "/big?n=5");
  EXPECT_EQ(ran("big"), 6);
  fetch(port, bhttp::verb::get, "/big?n=0");
  EXPECT_EQ(ran("big"), 7);
}

// Every route that lets browsers keep answers is in perf_report (037 § 9.4), and a hit's numbers
// say the cache answered.
TEST_F(CacheTest, PerfReportNamesBrowserCachingAndCountsHits) {
  ASSERT_EQ(fetch(port, bhttp::verb::get, "/big?n=1").status, 200);
  ASSERT_EQ(fetch(port, bhttp::verb::get, "/big?n=1").status, 200);
  auto r = tool("perf_report", {{"tenant", "shop"}});
  bool named = false;
  for (const auto& f : r.at("findings").as_array()) {
    if (f.at("kind") == "browser_cache" && f.at("route") == "mine") {
      named = true;
      EXPECT_EQ(f.at("browser_ms"), 30000);
    }
  }
  EXPECT_TRUE(named) << json::dump(r);
  EXPECT_EQ(r.at("routes").at(0).at("cache").at("hit"), 1) << json::dump(r.at("routes"));
}

TEST(CacheManifest, OnlyGetRoutesDeclareACacheAndItsFieldsAreChecked) {
  auto refused = [](const std::string& route, const char* why) {
    std::string err;
    std::string text = R"({"box": "b", "domains": ["*"], "routes": {"http": {"/x": )" + route + "}}}";
    EXPECT_FALSE(manifest::parse_manifest(text, &err)) << route;
    EXPECT_NE(err.find(why), std::string::npos) << err;
  };
  refused(R"({"post": {"name": "x", "public": true, "cache": {}}})", "only a get route");
  refused(R"({"get": {"name": "x", "public": true, "cache": {"by": "wake"}}})", "needs wake");
  refused(R"({"get": {"name": "x", "public": true, "cache": {"wake": ["a"]}}})", "only for by \"wake\"");
  refused(R"({"get": {"name": "x", "public": true, "cache": {"fresh-ms": 10}}})", "fresh-ms is 1000 to 86400000");
  refused(R"({"get": {"name": "x", "public": true, "cache": {"check-percent": 101}}})", "check-percent is 0 to 100");
  refused(R"({"get": {"name": "x", "public": true, "cache": {"by": "wake", "wake": ["u/{!user_id}"]}}})", "no {!user_id}");
  refused(R"({"get": {"name": "x", "public": true, "cache": {"ttl": 5}}})", "unknown field");
  std::string err;
  auto m = manifest::parse_manifest(
      R"({"box": "b", "domains": ["*"], "routes": {"http": {"/x": {"get": {"name": "x", "public": true, "cache": {}}}}}})", &err);
  ASSERT_TRUE(m) << err;
  const auto& c = m->routes[0].cache;
  EXPECT_TRUE(c.on);
  EXPECT_EQ(c.by, "data");
  EXPECT_EQ(c.fresh_ms, 60000);
  EXPECT_EQ(c.browser_ms, 0);
  EXPECT_EQ(c.check_percent, 1);
  auto sse = manifest::parse_manifest(
      R"({"box": "b", "domains": ["*"], "routes": {"sse": {"/x": {"subscribe": {"name": "x", "public": true, "wake": ["a"], "cache": {}}}}}})", &err);
  EXPECT_FALSE(sse);
}
