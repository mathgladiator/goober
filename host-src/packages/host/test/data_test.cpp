// End to end: schemas, a box that uses SQL, request transactions, and the operator's
// database tools (design/machine/008.Databases.md).

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/encoding.h"
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

// Three routes over one strategy: add a note, add one and then fail, and list them.
const char* kNotesWat = R"wat((module
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "notes")
  (data (i32.const 32) "INSERT INTO notes (text) VALUES (?)")
  (data (i32.const 80) "[\"hi\"]")
  (data (i32.const 96) "SELECT json_group_array(text) AS texts FROM notes")
  (data (i32.const 160) "[]")
  (func $answer (param $status i32) (param $n i32)
    (drop (call $result (i32.const 1024) (local.get $n)))
    (drop (call $respond (local.get $status) (i32.const 0) (i32.const 16) (i32.const 1024) (local.get $n))))
  (func (export "goober_route_add") (result i32)
    (call $answer (i32.const 201)
      (call $sql (i32.const 16) (i32.const 5) (i32.const 32) (i32.const 35) (i32.const 80) (i32.const 6)))
    (i32.const 0))
  (func (export "goober_route_boom") (result i32)
    (call $answer (i32.const 500)
      (call $sql (i32.const 16) (i32.const 5) (i32.const 32) (i32.const 35) (i32.const 80) (i32.const 6)))
    (i32.const 0))
  (func (export "goober_route_list") (result i32)
    (call $answer (i32.const 200)
      (call $sql (i32.const 16) (i32.const 5) (i32.const 96) (i32.const 49) (i32.const 160) (i32.const 2)))
    (i32.const 0))
))wat";

const char* kNotesManifest = R"({"box":"notes","domains":["*"],
  "data":{"sqlite3":{"notes":{"schema":"notes1","database":"notes","pattern":"sql",
    "tables":{"notes":{"read":true,"insert":true}}}}},
  "routes":{"http":{
    "/notes":{"post":{"name":"add","public":true},"get":{"name":"list","public":true}},
    "/boom":{"post":{"name":"boom","public":true}}}}})";

}  // namespace

class DataRoutesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "data_routes";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"localhost": {"tenant": "playground"}, "second.example": {"tenant": "second"}},
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

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(DataRoutesTest, BoxGuideDescribesTheBoundary) {
  auto guide = tool("box_guide", {});
  ASSERT_TRUE(guide.is_string()) << json::dump(guide);
  for (const char* must : {"goober_route_$name", "goober_sql", "additionalProperties", "routes", "timer_run", "schema_put"}) {
    EXPECT_NE(std::string(guide.as_string()).find(must), std::string::npos) << must;
  }
}

TEST_F(DataRoutesTest, BoxesWriteInATransactionAndOperatorsPoke) {
  std::string err;
  // A manifest naming a schema the host doesn't have is refused.
  EXPECT_FALSE(host->boxes().upload(kNotesManifest, kNotesWat, &err));
  EXPECT_NE(err.find("isn't stored"), std::string::npos) << err;
  auto s = tool("schema_put", {{"schema", bj::object{{"schema", "notes1"},
                                                     {"tables", bj::object{{"notes", bj::object{{"columns", bj::object{
                                                         {"text", bj::object{{"type", "string"}}}}}}}}}}}});
  ASSERT_FALSE(s.as_object().contains("error")) << json::dump(s);
  // Stored, with a warning: a table keyed by its rowid, not by a host id (038 § 5).
  ASSERT_TRUE(s.as_object().contains("warnings")) << json::dump(s);
  EXPECT_NE(json::dump(s.at("warnings")).find("names no primary key"), std::string::npos) << json::dump(s);
  ASSERT_TRUE(host->boxes().upload(kNotesManifest, kNotesWat, &err)) << err;

  // Before any request the database doesn't exist; db_list says who will use it.
  auto before = tool("db_list", {{"tenant", "playground"}});
  EXPECT_EQ(before.at(0).at("databases").at(0).at("exists"), false);
  EXPECT_EQ(before.at(0).at("databases").at(0).at("used_by").at(0).at("box"), "notes");

  EXPECT_EQ(fetch(port, bhttp::verb::post, "/notes").status, 201);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/notes").status, 201);
  // A box that answers 500 leaves nothing behind.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/boom").status, 500);
  auto list = fetch(port, bhttp::verb::get, "/notes");
  ASSERT_EQ(list.status, 200);
  EXPECT_NE(list.body.find(R"([\"hi\",\"hi\"])"), std::string::npos) << list.body;

  // Tenants are apart: the second tenant has its own, empty database.
  auto other = fetch(port, bhttp::verb::get, "/notes", {{"host", "second.example.localhost"}});
  EXPECT_NE(other.body.find(R"("rows":[["[]"]])"), std::string::npos) << other.body;

  // A form posted from another site never reaches the box.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/notes", {{"origin", "https://evil.example"}}).status, 403);

  // Operators poke.
  auto q = tool("db_query", {{"tenant", "playground"}, {"database", "notes"}, {"sql", "SELECT count(*) FROM notes"}});
  EXPECT_EQ(json::dump(q.at("rows")), "[[2]]");
  auto x = tool("db_exec", {{"tenant", "playground"}, {"database", "notes"}, {"sql", "UPDATE notes SET text = ?"},
                            {"params", bj::array{"edited"}}});
  EXPECT_EQ(x.at("changes"), 2);
  EXPECT_TRUE(tool("db_exec", {{"tenant", "playground"}, {"database", "notes"}, {"sql", "DROP TABLE notes"}})
                  .as_object()
                  .contains("error"));
  EXPECT_TRUE(tool("db_query", {{"tenant", "playground"}, {"database", "notes"}, {"sql", "DELETE FROM notes"}})
                  .as_object()
                  .contains("error"));
  auto log = base::read_file(root / "storage/logs/db.log");
  ASSERT_TRUE(log);
  EXPECT_NE(log->find("\"operator\":\"config\""), std::string::npos);
  auto d = tool("db_describe", {{"tenant", "playground"}, {"database", "notes"}});
  EXPECT_EQ(d.at("tables").at(0).at("rows"), 2);
  EXPECT_EQ(d.at("schemas").at(0).at("schema"), "notes1");

  // A new version whose schema would change a column is refused, naming the tenant.
  tool("schema_put", {{"schema", bj::object{{"schema", "notes2"},
                                           {"tables", bj::object{{"notes", bj::object{{"columns", bj::object{
                                               {"text", bj::object{{"type", "int64"}}}}}}}}}}}});
  std::string changed = kNotesManifest;
  changed.replace(changed.find("notes1"), 6, "notes2");
  EXPECT_FALSE(host->boxes().upload(changed, kNotesWat, &err));
  EXPECT_NE(err.find("tenant playground"), std::string::npos) << err;
}

TEST_F(DataRoutesTest, BoxesShareADatabaseAcrossTenants) {
  std::string err;
  auto put = [&](const char* name, const char* type) {
    return tool("schema_put", {{"schema", bj::object{{"schema", name},
                                                     {"tables", bj::object{{"notes", bj::object{{"columns", bj::object{
                                                         {"text", bj::object{{"type", type}}}}}}}}}}}});
  };
  ASSERT_FALSE(put("notes1", "string").as_object().contains("error"));
  ASSERT_FALSE(put("notes_bad", "int64").as_object().contains("error"));
  // Route names are unique across boxes, so each box's exports carry its name.
  auto wat = [](const std::string& box) {
    std::string w = kNotesWat;
    for (const char* r : {"add", "list", "boom"}) {
      std::string from = std::string("goober_route_") + r + "\"";
      w.replace(w.find(from), from.size(), std::string("goober_route_") + r + "_" + box + "\"");
    }
    return w;
  };
  auto manifest = [](const std::string& box, const std::string& domain, const std::string& schema) {
    return R"({"box":")" + box + R"(","domains":[")" + domain + R"("],
      "data":{"sqlite3":{"notes":{"schema":")" + schema + R"(","database":"notes","tenant":"shared","pattern":"sql",
        "tables":{"notes":{"read":true,"insert":true}}}}},
      "routes":{"http":{"/notes":{"post":{"name":"add_)" + box + R"(","public":true},"get":{"name":"list_)" + box + R"(","public":true}}}}})";
  };
  // A fixed tenant can't be combined with {!tenant} in the name, and must be a name.
  std::string bad = manifest("x", "localhost", "notes1");
  bad.replace(bad.find("\"shared\""), 8, "\"Not A Name\"");
  EXPECT_FALSE(host->boxes().upload(bad, wat("x"), &err));
  EXPECT_NE(err.find(".tenant"), std::string::npos) << err;

  ASSERT_TRUE(host->boxes().upload(manifest("notes", "localhost", "notes1"), wat("notes"), &err)) << err;
  ASSERT_TRUE(host->boxes().upload(manifest("notes_b", "second.example", "notes1"), wat("notes_b"), &err)) << err;
  // Two domains in two tenants write to one database.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/notes").status, 201);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/notes", {{"host", "second.example"}}).status, 201);
  auto list = fetch(port, bhttp::verb::get, "/notes", {{"host", "second.example"}});
  EXPECT_NE(list.body.find(R"([\"hi\",\"hi\"])"), std::string::npos) << list.body;
  EXPECT_TRUE(fs::exists(root / "storage" / "sqlite3" / "shared" / "databases" / "notes.db"));
  EXPECT_FALSE(fs::exists(root / "storage" / "sqlite3" / "playground" / "databases" / "notes.db"));

  // A third box bringing a schema that doesn't fit the shared database is refused.
  EXPECT_FALSE(host->boxes().upload(manifest("notes_c", "localhost", "notes_bad"), wat("notes_c"), &err));
  EXPECT_NE(err.find("notes"), std::string::npos) << err;
  // db_list shows the database under its own tenant, used by both boxes.
  auto dbs = tool("db_list", {{"tenant", "shared"}});
  EXPECT_NE(json::dump(dbs).find("notes_b"), std::string::npos) << json::dump(dbs);
}

// Grants against use (008 § 4): an upload is told when `true` covers a wide table, and
// grant_usage says what the box actually did, table by table, with what to narrow.
TEST_F(DataRoutesTest, GrantUsageComparesGrantsWithWhatRan) {
  std::string err;
  auto schema = json::parse(R"({"schema": "wide1", "tables": {
    "notes": {"columns": {"text": {"type": "string"}, "a": {"type": "string", "nullable": true},
      "b": {"type": "string", "nullable": true}, "c": {"type": "string", "nullable": true},
      "d": {"type": "string", "nullable": true}, "e": {"type": "string", "nullable": true},
      "f": {"type": "string", "nullable": true}, "g": {"type": "string", "nullable": true},
      "h": {"type": "string", "nullable": true}}},
    "extra": {"columns": {"x": {"type": "string"}}}}})", &err);
  ASSERT_TRUE(schema) << err;
  ASSERT_FALSE(tool("schema_put", {{"schema", *schema}}).as_object().contains("error"));
  const char* manifest = R"({"box":"notes","domains":["*"],
    "data":{"sqlite3":{"notes":{"schema":"wide1","database":"notes","pattern":"sql",
      "tables":{"notes":{"read":true,"insert":true,"update":true,"delete":true},"extra":{"read":["x"]}}}}},
    "routes":{"http":{"/notes":{"post":{"name":"add","public":true},"get":{"name":"list","public":true}},
      "/boom":{"post":{"name":"boom","public":true}}}}})";
  auto up = host->boxes().upload(manifest, kNotesWat, &err);
  ASSERT_TRUE(up) << err;
  std::string advice;
  for (const auto& w : up->warnings) advice += w + "\n";
  EXPECT_NE(advice.find("tables.notes: update: true lets the box change all 10 columns"), std::string::npos) << advice;
  EXPECT_EQ(advice.find("read: true"), std::string::npos) << advice;  // reads: grant_usage, with evidence
  EXPECT_EQ(advice.find("tables.extra"), std::string::npos) << advice;

  auto none = tool("grant_usage", {{"box", "notes"}});
  EXPECT_EQ(none.at(0).at("strategies").at(0).at("tables").at(1).at("findings").at(0).at("kind"), "unused_table");

  EXPECT_EQ(fetch(port, bhttp::verb::post, "/notes").status, 201);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/notes").status, 200);
  auto report = tool("grant_usage", {{"box", "notes"}});
  ASSERT_TRUE(report.is_array()) << json::dump(report);
  const auto& tables = report.at(0).at("strategies").at(0).at("tables").as_array();
  // Sorted by table name: extra, then notes.
  EXPECT_EQ(tables.at(0).at("table"), "extra");
  EXPECT_EQ(tables.at(0).at("calls"), 0);
  EXPECT_EQ(tables.at(0).at("findings").at(0).at("kind"), "unused_table");
  const auto& notes = tables.at(1).as_object();
  EXPECT_EQ(notes.at("calls"), 2);
  EXPECT_EQ(notes.at("granted").at("ops"), "riud");
  EXPECT_EQ(json::dump(notes.at("used").at("read")), R"(["text"])");
  EXPECT_EQ(notes.at("used").at("insert"), true);
  std::map<std::string, bj::value> kinds;
  for (const auto& f : notes.at("findings").as_array()) kinds[std::string(f.at("kind").as_string())] = f;
  ASSERT_TRUE(kinds.count("unused_write")) << json::dump(notes);
  EXPECT_EQ(json::dump(kinds["unused_write"].at("operations")), R"(["update","delete"])");
  ASSERT_TRUE(kinds.count("narrow_read")) << json::dump(notes);
  EXPECT_EQ(json::dump(kinds["narrow_read"].at("suggested")), R"(["text"])");
  EXPECT_EQ(report.at(0).at("findings"), 3);
  EXPECT_TRUE(tool("grant_usage", {{"box", "nope"}}).as_object().contains("error"));
}

// Jobs (design/machine/038.Ids.md § 7): a box that moves rows from a table keyed the old way
// into one keyed by host ids, in batches of two. A real run is refused until a dry run passed;
// the dry run works on copies and leaves the real database alone; the real run finishes,
// records done, and stamps host ids; a job that keeps failing fails its dry run.
const char* kMoveWat = R"wat((module
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "s")
  (data (i32.const 64) "INSERT INTO legacy (name) VALUES ('a'), ('b'), ('c'), ('d'), ('e')")
  (data (i32.const 160) "INSERT INTO members (old_id, name) SELECT id, name FROM legacy WHERE id NOT IN (SELECT old_id FROM members) ORDER BY id LIMIT 2")
  (data (i32.const 304) "SELECT json_object('cursor', CAST(coalesce((SELECT max(old_id) FROM members), 0) AS TEXT), 'done', json(CASE WHEN EXISTS (SELECT 1 FROM legacy WHERE id NOT IN (SELECT old_id FROM members)) THEN 'false' ELSE 'true' END), 'moved', 2) AS a")
  (data (i32.const 560) "[]")
  (data (i32.const 592) "{\"error\":\"broken on purpose\"}")
  ;; The first value of the result's first row, unescaped, into 12288: from after [[" to the closing ".
  (func $first (param $n i32) (result i32)
    (local $i i32) (local $o i32) (local $c i32)
    (drop (call $result (i32.const 4096) (local.get $n)))
    (local.set $i (i32.const 4096))
    (block $found (loop $seek
      (br_if $found (i32.and (i32.eq (i32.load8_u (local.get $i)) (i32.const 91))
                             (i32.and (i32.eq (i32.load8_u (i32.add (local.get $i) (i32.const 1))) (i32.const 91))
                                      (i32.eq (i32.load8_u (i32.add (local.get $i) (i32.const 2))) (i32.const 34)))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $seek)))
    (local.set $i (i32.add (local.get $i) (i32.const 3)))
    (local.set $o (i32.const 12288))
    (block $end (loop $copy
      (local.set $c (i32.load8_u (local.get $i)))
      (br_if $end (i32.eq (local.get $c) (i32.const 34)))
      (if (i32.eq (local.get $c) (i32.const 92))
        (then (local.set $i (i32.add (local.get $i) (i32.const 1)))
              (local.set $c (i32.load8_u (local.get $i)))))
      (i32.store8 (local.get $o) (local.get $c))
      (local.set $o (i32.add (local.get $o) (i32.const 1)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $copy)))
    (i32.sub (local.get $o) (i32.const 12288)))
  (func (export "goober_route_seed_legacy") (result i32)
    (drop (call $sql (i32.const 32) (i32.const 1) (i32.const 64) (i32.const 66) (i32.const 560) (i32.const 2)))
    (drop (call $respond (i32.const 204) (i32.const 0) (i32.const 16) (i32.const 0) (i32.const 0)))
    (i32.const 0))
  (func (export "goober_route_move_members") (result i32)
    (local $n i32)
    (drop (call $sql (i32.const 32) (i32.const 1) (i32.const 160) (i32.const 127) (i32.const 560) (i32.const 2)))
    (local.set $n (call $first (call $sql (i32.const 32) (i32.const 1) (i32.const 304) (i32.const 236) (i32.const 560) (i32.const 2))))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 12288) (local.get $n)))
    (i32.const 0))
  (func (export "goober_route_always_fails") (result i32)
    (drop (call $respond (i32.const 500) (i32.const 0) (i32.const 16) (i32.const 592) (i32.const 29)))
    (i32.const 0))
))wat";

const char* kMoveManifest = R"({"box":"mover","domains":["*"],
  "data":{"sqlite3":{"s":{"schema":"move1","database":"main","pattern":"sql",
    "tables":{"legacy":{"read":true,"insert":true},"members":{"read":true,"insert":true}}}}},
  "routes":{"http":{"/seed":{"post":{"name":"seed_legacy","public":true}}},
            "job":{"move":{"name":"move_members","timeout-ms":2000},"broken":{"name":"always_fails"}}}})";

TEST_F(DataRoutesTest, JobsDryRunThenMoveRowsInBatches) {
  auto put = tool("schema_put", {{"schema", R"({"schema":"move1","tables":{
      "legacy":{"columns":{"name":{"type":"string"}}},
      "members":{"columns":{"id":{"type":"id","primary-key":true},"old_id":{"type":"int64"},"name":{"type":"string"}},
                 "indexes":{"members_by_old":{"columns":["old_id"],"unique":true}}}}})"}});
  ASSERT_FALSE(put.as_object().contains("error")) << json::dump(put);
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kMoveManifest, kMoveWat, &err)) << err;
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/seed").status, 204);
  auto count = [&](const char* table) {
    auto r = tool("db_query", {{"tenant", "playground"}, {"database", "main"}, {"sql", std::string("SELECT count(*) FROM ") + table}});
    return r.at("rows").at(0).at(0).as_int64();
  };
  ASSERT_EQ(count("legacy"), 5);

  auto start = [&](const char* tool_name, const char* job, bj::object more = {}) {
    bj::object args{{"box", "mover"}, {"job", job}, {"tenant", "playground"}};
    for (auto& kv : more) args[kv.key()] = kv.value();
    return tool(tool_name, std::move(args));
  };
  // No real run before a dry run has passed.
  auto refused = start("job_start", "move_members");
  ASSERT_TRUE(refused.as_object().contains("error"));
  EXPECT_NE(std::string(refused.at("error").as_string()).find("job_dry_run first"), std::string::npos);

  // The dry run: three batches (2, 2, 1), on copies; the real table is untouched.
  auto dry = start("job_dry_run", "move_members");
  ASSERT_EQ(dry.at("state"), "passed") << json::dump(dry);
  EXPECT_EQ(dry.at("calls"), 3);
  EXPECT_EQ(dry.at("dry_run"), true);
  EXPECT_EQ(count("members"), 0);
  EXPECT_FALSE(fs::exists(root / "storage" / "tmp" / "jobs" / std::string(dry.at("run").as_string())));

  // The real run: the same batches, for real, with host ids.
  auto run = start("job_start", "move_members");
  ASSERT_EQ(run.at("state"), "done") << json::dump(run);
  EXPECT_EQ(run.at("cursor"), "5");
  EXPECT_EQ(run.at("moved"), 6);  // the box said 2 per batch
  EXPECT_EQ(count("members"), 5);
  auto ids = tool("db_query", {{"tenant", "playground"}, {"database", "main"}, {"sql", "SELECT min(id) >= 4294967296 FROM members"}});
  EXPECT_EQ(ids.at("rows").at(0).at(0), 1);
  // Done is recorded: again only on purpose, and then it changes nothing (the box is idempotent).
  auto twice = start("job_start", "move_members");
  ASSERT_TRUE(twice.as_object().contains("error"));
  EXPECT_NE(std::string(twice.at("error").as_string()).find("again: true"), std::string::npos);
  auto again = start("job_start", "move_members", {{"again", true}});
  EXPECT_EQ(again.at("state"), "done") << json::dump(again);
  EXPECT_EQ(count("members"), 5);

  // A job that keeps failing fails its dry run after three tries, saying why.
  auto broken = start("job_dry_run", "always_fails");
  ASSERT_EQ(broken.at("state"), "failed") << json::dump(broken);
  EXPECT_EQ(broken.at("calls"), 3);
  EXPECT_NE(json::dump(broken.at("last_error")).find("500"), std::string::npos) << json::dump(broken);

  // The list says where each job stands, and a run's status reads back.
  auto list = tool("job_list", {{"tenant", "playground"}});
  ASSERT_EQ(list.at("jobs").as_array().size(), 2u) << json::dump(list);
  for (const auto& j : list.at("jobs").as_array()) {
    if (j.at("job") == "move_members") {
      EXPECT_EQ(j.at("done"), true);
      EXPECT_EQ(j.at("dry_run_passed"), true);
    }
  }
  auto status = tool("job_status", {{"run", run.at("run")}});
  EXPECT_EQ(status.at("state"), "done");
  // Job routes have no address: nothing on the web reaches them.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/move_members").status, 404);
}

// schema_check: everything schema_put checks, nothing stored, so a name is never spent on a try.
TEST_F(DataRoutesTest, SchemaCheckStoresNothing) {
  const char* text = R"({"schema":"tries1","tables":{"seen":{"columns":{
      "id":{"type":"id","primary-key":true},"addr":{"type":"string"},"day":{"type":"string"},"user_id":{"type":"string"}},
      "indexes":{"seen_by_addr_day":{"columns":["addr","day"],"unique":true}}}}})";
  auto c = tool("schema_check", {{"schema", text}});
  ASSERT_EQ(c.at("ok"), true) << json::dump(c);
  EXPECT_EQ(c.at("stored"), false);
  EXPECT_NE(json::dump(c.at("warnings")).find("seen.user_id looks like a reference"), std::string::npos) << json::dump(c);
  EXPECT_EQ(json::dump(tool("schema_list", {})).find("tries1"), std::string::npos);
  // A natural key as the primary key is refused, saying what to do.
  auto bad = tool("schema_check", {{"schema", R"({"schema":"tries2","tables":{"seen":{"columns":{
      "key":{"type":"string","primary-key":true}}}}})"}});
  ASSERT_TRUE(bad.as_object().contains("error"));
  EXPECT_NE(std::string(bad.at("error").as_string()).find("unique index"), std::string::npos) << json::dump(bad);
  // Once stored, the same content checks out as stored; other content under the name doesn't.
  ASSERT_FALSE(tool("schema_put", {{"schema", text}}).as_object().contains("error"));
  EXPECT_EQ(tool("schema_check", {{"schema", text}}).at("stored"), true);
  EXPECT_TRUE(tool("schema_check", {{"schema", R"({"schema":"tries1","tables":{"x":{"columns":{"a":{"type":"string"}}}}})"}})
                  .as_object().contains("error"));
}
