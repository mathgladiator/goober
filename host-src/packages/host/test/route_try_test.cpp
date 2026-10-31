// route_try: a box version tried before it's live (route_try.cpp). A dark version answers
// through route_try while live traffic still gets the active one; the gate answers as for a
// real request; a write runs on copies and changes no real data; a call to another box is
// refused in a write's try; and the access log marks every try.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <fstream>

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
  return Reply{int(res.result_int()), res.body()};
}

// notes: /hello answers which version it is; /secret is for members; /add inserts a note.
std::string notes_wat(const std::string& which) {
  return R"wat((module
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_log" (func $log (param i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "notes")
  (data (i32.const 32) "INSERT INTO notes (text) VALUES ('hi')")
  (data (i32.const 80) "[]")
  (data (i32.const 96) "{\22v\22:\22)wat" + which + R"wat(\22}")
  (data (i32.const 128) "{\22secret\22:true}")
  (data (i32.const 160) "adding a note")
  (func (export "goober_route_hello") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 96) (i32.const )wat" +
         std::to_string(8 + which.size()) + R"wat()))
    (i32.const 0))
  (func (export "goober_route_secret") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 128) (i32.const 15)))
    (i32.const 0))
  (func (export "goober_route_add") (result i32)
    (drop (call $log (i32.const 1) (i32.const 160) (i32.const 13)))
    (drop (call $sql (i32.const 16) (i32.const 5) (i32.const 32) (i32.const 38) (i32.const 80) (i32.const 2)))
    (drop (call $respond (i32.const 201) (i32.const 0) (i32.const 16) (i32.const 80) (i32.const 2)))
    (i32.const 0))
))wat";
}

const char* kNotesManifest = R"({"box":"notes","domains":["*"],
  "data":{"sqlite3":{"notes":{"schema":"tries1","database":"notes","pattern":"sql",
    "tables":{"notes":{"read":true,"insert":true}}}}},
  "routes":{"http":{
    "/hello":{"get":{"name":"hello","public":true}},
    "/secret":{"get":{"name":"secret","requirements":[["member"]]}},
    "/add":{"post":{"name":"add","public":true}}}}})";

// billing answers a ping for jobs; jobs' public route calls it.
const char* kBilling = R"wat((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22pong\22:true}")
  (func (export "goober_route_ping") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 13)))
    (i32.const 0))
))wat";
const char* kJobs = R"wat((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "billing")
  (data (i32.const 48) "ping")
  (data (i32.const 128) "{}")
  (func (export "goober_route_run_ping") (result i32)
    (local $n i32)
    (local.set $n (call $call (i32.const 16) (i32.const 7) (i32.const 48) (i32.const 4) (i32.const 128) (i32.const 2)))
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 4096) (local.get $n)))
    (i32.const 0))
))wat";
const char* kBillingManifest = R"({"box":"billing","domains":["*"],
  "routes":{"internal":{"/ping":{"get":{"name":"ping","callers":["jobs"]}}}}})";
const char* kJobsManifest = R"({"box":"jobs","domains":["*"],
  "external":{"billing":{"box":"billing","methods":{"ping":{}}}},
  "routes":{"http":{"/run-ping":{"post":{"name":"run_ping","public":true}}}}})";

}  // namespace

class RouteTryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "route_try";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"localhost": {"tenant": "playground"}},
        "operations": {"token": "test-token-0123456789"},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp", {{"authorization", "Bearer test-token-0123456789"}},
                   json::dump(req));
    std::string err;
    auto doc = json::parse(r.body, &err);
    const auto& result = doc->at("result");
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::object{{"error", text}};
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  }
  bj::value try_route(bj::object args) {
    args["domain"] = "localhost";
    if (!args.contains("box")) args["box"] = "notes";
    return tool("route_try", std::move(args));
  }
  std::string header(const bj::value& r, const std::string& name) {
    for (const auto& h : r.at("headers").as_array()) {
      if (h.at(0).as_string() == name) return std::string(h.at(1).as_string());
    }
    return "";
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(RouteTryTest, ADarkVersionAnswersThroughATryWhileLiveTrafficGetsTheActive) {
  auto put = tool("schema_put", {{"schema", R"({"schema":"tries1","tables":{"notes":{"columns":{"text":{"type":"string"}}}}})"}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kNotesManifest, notes_wat("one"), &err)) << err;
  auto dark = host->boxes().store(kNotesManifest, notes_wat("two"), "", &err);
  ASSERT_TRUE(dark) << err;

  EXPECT_NE(fetch(port, bhttp::verb::get, "/hello").body.find("\"one\""), std::string::npos);
  auto tried = try_route({{"version", dark->version}, {"path", "/hello"}});
  ASSERT_FALSE(tried.as_object().contains("error")) << json::dump(tried);
  EXPECT_EQ(tried.at("status"), 200);
  EXPECT_EQ(tried.at("version").as_string(), dark->version);
  EXPECT_NE(std::string(tried.at("body").as_string()).find("\"two\""), std::string::npos);
  EXPECT_FALSE(header(tried, "etag").empty());  // what the host adds, a real GET's headers
  // Live traffic is unchanged: the try made nothing live.
  EXPECT_NE(fetch(port, bhttp::verb::get, "/hello").body.find("\"one\""), std::string::npos);
  // The active version by default.
  EXPECT_NE(std::string(try_route({{"path", "/hello"}}).at("body").as_string()).find("\"one\""), std::string::npos);
  // A path the version doesn't have.
  EXPECT_TRUE(try_route({{"path", "/nope"}}).as_object().contains("error"));
}

TEST_F(RouteTryTest, TheGateAnswersAsForARealRequest) {
  tool("schema_put", {{"schema", R"({"schema":"tries1","tables":{"notes":{"columns":{"text":{"type":"string"}}}}})"}});
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kNotesManifest, notes_wat("one"), &err)) << err;
  tool("role_grant", {{"tenant", "playground"}, {"email", "member@example.com"}, {"role", "member"}});
  tool("role_grant", {{"tenant", "playground"}, {"email", "other@example.com"}, {"role", "editor"}});

  EXPECT_EQ(try_route({{"path", "/secret"}}).at("status"), 401);  // an API caller
  auto browser = try_route({{"path", "/secret"}, {"headers", bj::object{{"accept", "text/html"}}}});
  EXPECT_EQ(browser.at("status"), 303);
  EXPECT_NE(header(browser, "location").find("/identity/sign-in"), std::string::npos);
  auto member = try_route({{"path", "/secret"}, {"as", bj::object{{"email", "member@example.com"}}}});
  EXPECT_EQ(member.at("status"), 200) << json::dump(member);
  EXPECT_EQ(member.at("as"), "member@example.com");
  EXPECT_EQ(try_route({{"path", "/secret"}, {"as", bj::object{{"email", "other@example.com"}}}}).at("status"), 403);
  EXPECT_TRUE(try_route({{"path", "/secret"}, {"as", bj::object{{"email", "nobody@example.com"}}}}).as_object().contains("error"));
}

TEST_F(RouteTryTest, AWriteRunsOnCopiesAndChangesNoRealData) {
  tool("schema_put", {{"schema", R"({"schema":"tries1","tables":{"notes":{"columns":{"text":{"type":"string"}}}}})"}});
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kNotesManifest, notes_wat("one"), &err)) << err;
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/add", {{"origin", "http://localhost"}}).status, 201);
  auto count = [&] {
    auto r = tool("db_query", {{"tenant", "playground"}, {"database", "notes"}, {"sql", "SELECT count(*) FROM notes"}});
    return r.at("rows").at(0).at(0).as_int64();
  };
  ASSERT_EQ(count(), 1);

  auto tried = try_route({{"method", "post"}, {"path", "/add"}});
  ASSERT_FALSE(tried.as_object().contains("error")) << json::dump(tried);
  EXPECT_EQ(tried.at("status"), 201);
  EXPECT_EQ(tried.at("changes").at("notes/notes").at("rows_before"), 1);
  EXPECT_EQ(tried.at("changes").at("notes/notes").at("rows_after"), 2);
  EXPECT_EQ(tried.at("used").at("notes").at("notes").at("insert"), true);
  EXPECT_EQ(tried.at("logs").at(0).at("text"), "adding a note");
  EXPECT_EQ(count(), 1);  // the real table is as it was
  std::error_code ec;
  int left = 0;
  for (auto it = fs::directory_iterator(root / "storage" / "tmp" / "tries", ec); !ec && it != fs::directory_iterator(); ++it) ++left;
  EXPECT_EQ(left, 0);  // the copies are gone

  // The access log marks it a try, so traffic and performance leave it out.
  std::ifstream in(root / "storage" / "logs" / "access.log");
  std::string line;
  bool marked = false;
  while (std::getline(in, line)) {
    if (line.find("\"try\":true") != std::string::npos && line.find("\"route\":\"add\"") != std::string::npos) marked = true;
  }
  EXPECT_TRUE(marked);
}

TEST_F(RouteTryTest, AWritesTryCantCallAnotherBox) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kBillingManifest, kBilling, &err)) << err;
  ASSERT_TRUE(host->boxes().upload(kJobsManifest, kJobs, &err)) << err;
  auto tried = try_route({{"box", "jobs"}, {"method", "post"}, {"path", "/run-ping"}});
  ASSERT_FALSE(tried.as_object().contains("error")) << json::dump(tried);
  EXPECT_NE(std::string(tried.at("body").as_string()).find("a dry run reaches only copies"), std::string::npos)
      << json::dump(tried);
  // For real, the call goes through.
  auto real = fetch(port, bhttp::verb::post, "/run-ping", {{"origin", "http://localhost"}});
  EXPECT_NE(real.body.find("pong"), std::string::npos) << real.body;
}
