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
