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
        "domains": {"localhost": {"tenant": "playground"}, "example.com": {"tenant": "playground"}},
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

// A tool and a mail route tried as the site's /mcp and the MTA hook would call them: always on
// copies (they may write), a tool as an account only, mail as nobody with its sender unverified.
const char* kToolsAndMailManifest = R"({"box":"notes","domains":["*"],
  "data":{"sqlite3":{"notes":{"schema":"tries1","database":"notes","pattern":"sql",
    "tables":{"notes":{"read":true,"insert":true}}}}},
  "routes":{"mcp":{"add":{"tool":{"description":"Adds a note.","requirements":[["member"]]}}},
            "mail":{"notes@":{"receive":{"name":"hello","public":true}}}}})";

TEST_F(RouteTryTest, ToolsAndMailRoutesAreTriedOnCopies) {
  tool("schema_put", {{"schema", R"({"schema":"tries1","tables":{"notes":{"columns":{"text":{"type":"string"}}}}})"}});
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kToolsAndMailManifest, notes_wat("one"), &err)) << err;
  tool("role_grant", {{"tenant", "playground"}, {"email", "member@example.com"}, {"role", "member"}});
  tool("role_grant", {{"tenant", "playground"}, {"email", "other@example.com"}, {"role", "editor"}});
  auto count = [&]() -> int64_t {
    auto r = tool("db_query", {{"tenant", "playground"}, {"database", "notes"}, {"sql", "SELECT count(*) FROM notes"}});
    return r.as_object().contains("rows") ? r.at("rows").at(0).at(0).as_int64() : 0;
  };

  // One kind of call at a time.
  EXPECT_TRUE(try_route({{"path", "/x"}, {"tool", "add"}}).as_object().contains("error"));
  // A tool acts for somebody; one whose roles fall short isn't listed, and the box doesn't run.
  EXPECT_TRUE(try_route({{"tool", "add"}}).as_object().contains("error"));
  auto hidden = try_route({{"tool", "add"}, {"as", bj::object{{"email", "other@example.com"}}}});
  ASSERT_TRUE(hidden.as_object().contains("listed")) << json::dump(hidden);
  EXPECT_EQ(hidden.at("listed"), false) << json::dump(hidden);
  // A member's call runs on copies: the box's insert shows, the real table is untouched.
  auto added = try_route({{"tool", "add"}, {"as", bj::object{{"email", "member@example.com"}}}});
  ASSERT_FALSE(added.as_object().contains("error")) << json::dump(added);
  EXPECT_EQ(added.at("status"), 201);
  EXPECT_EQ(added.at("is_error"), false);
  EXPECT_EQ(added.at("logs").at(0).at("text"), "adding a note");
  EXPECT_EQ(added.at("used").at("notes").at("notes").at("insert"), true) << json::dump(added);
  EXPECT_EQ(count(), 0);
  // Arguments a tool without an input type doesn't take are refused as /mcp refuses them.
  auto extra = try_route({{"tool", "add"}, {"arguments", bj::object{{"x", 1}}}, {"as", bj::object{{"email", "member@example.com"}}}});
  EXPECT_EQ(extra.at("error"), "this tool takes no input") << json::dump(extra);

  // Mail: the route's answer becomes the SMTP reply; the sender is never verified in a try.
  auto mail = try_route({{"mail", bj::object{{"to", "notes@example.com"}, {"from", "Ada <ada@example.org>"},
                                             {"subject", "Hello"}, {"text", "Hi there"}}}});
  ASSERT_FALSE(mail.as_object().contains("error")) << json::dump(mail);
  EXPECT_EQ(mail.at("route"), "hello");
  EXPECT_EQ(mail.at("outcome"), "delivered");
  EXPECT_EQ(mail.at("smtp").at("status"), 250);
  EXPECT_EQ(mail.at("sender_verified"), false);
  EXPECT_FALSE(mail.at("sender_why").is_null());
  // An address no route claims is refused at RCPT, before any box runs.
  auto nobody = try_route({{"mail", bj::object{{"to", "nobody@example.com"}, {"from", "ada@example.org"}, {"text", "x"}}}});
  EXPECT_EQ(nobody.at("outcome"), "no_route") << json::dump(nobody);
  EXPECT_EQ(nobody.at("smtp").at("status"), 550);
  // Mail always arrives as nobody.
  EXPECT_TRUE(try_route({{"mail", bj::object{{"to", "notes@example.com"}, {"from", "ada@example.org"}}},
                         {"as", bj::object{{"email", "member@example.com"}}}})
                  .as_object()
                  .contains("error"));

  // The access log marks both.
  std::ifstream in(root / "storage" / "logs" / "access.log");
  std::string line;
  bool tool_marked = false, mail_marked = false;
  while (std::getline(in, line)) {
    if (line.find("\"try\":true") == std::string::npos) continue;
    tool_marked |= line.find("\"tool\":\"add\"") != std::string::npos;
    mail_marked |= line.find("\"mail_try\":\"delivered\"") != std::string::npos;
  }
  EXPECT_TRUE(tool_marked);
  EXPECT_TRUE(mail_marked);
}

namespace {

// clock: beat writes a tick; remind writes one for the reminder row it's given.
const char* kClockWat = R"wat((module
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "w")
  (data (i32.const 16) "INSERT INTO ticks (what) VALUES (?)")
  (data (i32.const 64) "[\22beat\22]")
  (data (i32.const 96) "[\22remind\22]")
  (func $tick (param $p i32) (param $n i32)
    (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 35) (local.get $p) (local.get $n))))
  (func (export "goober_route_beat") (result i32) (call $tick (i32.const 64) (i32.const 8)) (i32.const 0))
  (func (export "goober_route_remind") (result i32) (call $tick (i32.const 96) (i32.const 10)) (i32.const 0))
))wat";
const char* kClockSchema = R"({"schema":"clock2","tables":{
  "ticks":{"columns":{"what":{"type":"string","max-length":40}}},
  "reminders":{"columns":{"remind_at":{"type":"int64"},"on":{"type":"boolean","default":true}}}}})";
const char* kClockManifest = R"({"box":"clock","domains":["*"],
  "data":{"sqlite3":{
    "w":{"schema":"clock2","database":"clock","pattern":"sql","tables":{"ticks":{"read":true,"insert":true}}},
    "remind_t":{"schema":"clock2","database":"clock","pattern":"by_timer","table":"reminders","active-field":"on"}}},
  "routes":{"timer":{
    "beat":{"name":"beat","every-ms":60000},
    "remind":{"name":"remind","source":"remind_t","at-field":"remind_at"}}}})";

}  // namespace

TEST_F(RouteTryTest, TimersFireOnceOnCopiesAtTheGivenTime) {
  auto put = tool("schema_put", {{"schema", kClockSchema}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  std::string err;
  auto dark = host->boxes().store(kClockManifest, kClockWat, "", &err);
  ASSERT_TRUE(dark) << err;
  auto schema = host->schemas().get("clock2");
  ASSERT_TRUE(host->databases().open("playground", "clock", &*schema, &err)) << err;
  ASSERT_TRUE(host->databases().operate("playground", "clock",
                                        "INSERT INTO reminders (remind_at) VALUES (2000000000000), (1900000000000)",
                                        nullptr, 10, true, &err))
      << err;
  auto real = [&](const std::string& sql) {
    auto r = host->databases().operate("playground", "clock", sql, nullptr, 10, false, &err);
    return r ? r->at("rows").at(0).at(0) : bj::value(nullptr);
  };

  // A plain timer of a version stored dark: fired once, on copies.
  auto beat = try_route({{"box", "clock"}, {"version", dark->version}, {"timer", "beat"}, {"at_ms", "1950000000000"}});
  ASSERT_FALSE(beat.as_object().contains("error")) << json::dump(beat);
  EXPECT_EQ(beat.at("ok"), true);
  EXPECT_EQ(beat.at("at"), beat.at("due"));
  EXPECT_EQ(beat.at("changes").at("clock/ticks").at("rows_after"), 1);
  EXPECT_EQ(real("SELECT count(*) FROM ticks"), 0);

  // The row due first fires, and is turned off in the copies only.
  auto remind = try_route({{"box", "clock"}, {"version", dark->version}, {"timer", "remind"}, {"at_ms", 1950000000000}});
  ASSERT_FALSE(remind.as_object().contains("error")) << json::dump(remind);
  EXPECT_EQ(remind.at("due"), "2030-03-17T17:46:40.000Z");
  EXPECT_FALSE(remind.as_object().contains("due_note"));
  EXPECT_EQ(remind.at("ok"), true);
  EXPECT_EQ(real("SELECT count(*) FROM reminders WHERE \"on\" = 1"), 2);

  // A named row, not due yet at at_ms: fired anyway, and said so.
  auto id = real("SELECT id FROM reminders WHERE remind_at = 2000000000000");
  std::string later = id.is_string() ? std::string(id.as_string()) : json::dump(id);
  auto named = try_route({{"box", "clock"}, {"version", dark->version}, {"timer", "remind"}, {"row", later},
                          {"at_ms", 1950000000000}});
  ASSERT_FALSE(named.as_object().contains("error")) << json::dump(named);
  EXPECT_EQ(named.at("row").as_string(), later);
  EXPECT_TRUE(named.as_object().contains("due_note"));

  // A timer fires as nobody; an unknown timer or row is refused.
  EXPECT_TRUE(try_route({{"box", "clock"}, {"version", dark->version}, {"timer", "beat"},
                         {"as", bj::object{{"email", "a@example.com"}}}}).as_object().contains("error"));
  EXPECT_TRUE(try_route({{"box", "clock"}, {"version", dark->version}, {"timer", "nope"}}).as_object().contains("error"));
  EXPECT_TRUE(try_route({{"box", "clock"}, {"version", dark->version}, {"timer", "remind"}, {"row", "nope"}})
                  .as_object().contains("error"));
}

TEST_F(RouteTryTest, PromotingAnUntriedVersionWarnsButGoesLive) {
  tool("schema_put", {{"schema", R"({"schema":"tries1","tables":{"notes":{"columns":{"text":{"type":"string"}}}}})"}});
  std::string err;
  auto one = host->boxes().upload(kNotesManifest, notes_wat("one"), &err);
  ASSERT_TRUE(one) << err;
  auto two = host->boxes().store(kNotesManifest, notes_wat("two"), "", &err);
  auto three = host->boxes().store(kNotesManifest, notes_wat("three"), "", &err);
  ASSERT_TRUE(two && three) << err;

  auto promoted = tool("version_promote", {{"box", "notes"}, {"version", two->version}});
  ASSERT_FALSE(promoted.as_object().contains("error")) << json::dump(promoted);
  EXPECT_EQ(promoted.at("active").as_string(), two->version);  // never refused
  ASSERT_TRUE(promoted.as_object().contains("warnings"));
  EXPECT_NE(std::string(promoted.at("warnings").at(0).as_string()).find("untried"), std::string::npos);

  // Going back to a version that ran live is a rollback, not an untried version.
  auto back = tool("version_promote", {{"box", "notes"}, {"version", one->version}});
  EXPECT_FALSE(back.as_object().contains("warnings")) << json::dump(back);
  EXPECT_EQ(back.at("tried").at(0).at("kind"), "live");

  // A version tried first goes live quietly, and says what tried it.
  ASSERT_EQ(try_route({{"version", three->version}, {"path", "/hello"}}).at("status"), 200);
  auto tried = tool("version_promote", {{"box", "notes"}, {"version", three->version}});
  EXPECT_FALSE(tried.as_object().contains("warnings")) << json::dump(tried);
  EXPECT_EQ(tried.at("tried").at(0).at("kind"), "http");
  EXPECT_EQ(tried.at("tried").at(0).at("tenant"), "playground");
}
