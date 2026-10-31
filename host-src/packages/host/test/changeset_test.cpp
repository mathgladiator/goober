// Change sets (design/machine/042.ChangeSets.md; changesets.cpp): a schema, a box version,
// content, and a flag applied in one call and rolled back in one call; an apply that fails
// partway undoes what it applied; a preview stores nothing; a rollback over a teammate's later
// change is refused unless forced.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

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

// shop: GET /mode answers its "mode" flag; `which` makes each version's module differ.
std::string shop_wat(const std::string& which) {
  return R"((module
  (import "goober" "goober_flag" (func $flag (param i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "text/plain")
  (data (i32.const 16) "mode")
  (data (i32.const 2000) ")" + which + R"(")
  (func (export "goober_route_mode") (result i32)
    (local $n i32)
    (local.set $n (call $flag (i32.const 16) (i32.const 4)))
    (drop (call $result (i32.const 1024) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 10) (i32.const 1024) (local.get $n)))
    (i32.const 0))
))";
}

const char* kShopV1 = R"({"box":"shop","domains":["*"],
  "flags":{"mode":{"default":"open","values":["open","closed"]}},
  "routes":{"http":{"/mode":{"get":{"name":"mode","public":true}}}}})";
// v2 keeps notes in a table of its own and lets the flag say "sale".
const char* kShopV2 = R"({"box":"shop","domains":["*"],
  "flags":{"mode":{"default":"open","values":["open","closed","sale"]}},
  "data":{"sqlite3":{"notes":{"schema":"shopnotes1","database":"shop","pattern":"sql",
    "tables":{"notes":{"read":true,"insert":true}}}}},
  "routes":{"http":{"/mode":{"get":{"name":"mode","public":true}}}}})";
const char* kNotesSchema = R"({"schema":"shopnotes1","tables":{"notes":{"columns":{
  "id":{"type":"id","primary-key":true},"text":{"type":"string"}}}}})";

// other: also GET /mode, so promoting it while shop is live clashes.
const char* kOther = R"({"box":"other","domains":["*"],
  "flags":{"mode":{"default":"x"}},
  "routes":{"http":{"/mode":{"get":{"name":"mode","public":true}}}}})";

}  // namespace

class ChangeSetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "changesets";
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
    if (result.at("isError").as_bool()) return bj::object{{"tool_error", text}};
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  }
  std::string ok(const bj::value& v) { return v.is_object() && v.as_object().contains("tool_error") ? json::dump(v) : ""; }
  bool refused(const bj::value& v) { return v.is_object() && v.as_object().contains("tool_error"); }
  std::string live(const std::string& box) {
    auto active = host->boxes().active();
    auto it = active->boxes.find(box);
    return it == active->boxes.end() ? "" : it->second->version;
  }
  std::string create(const std::string& title) {
    auto c = tool("changeset_create", {{"title", title}, {"why", "testing"}});
    EXPECT_EQ(ok(c), "");
    return std::string(c.at("id").as_string());
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(ChangeSetTest, AppliesEverythingInOneCallAndRollsItAllBack) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kShopV1, shop_wat("one"), &err)) << err;
  std::string v1 = live("shop");
  ASSERT_EQ(fetch(port, bhttp::verb::get, "/mode").body, "open");
  ASSERT_EQ(fetch(port, bhttp::verb::get, "/about").status, 404);

  std::string id = create("sale mode");
  auto added = tool(
      "changeset_add",
      {{"id", id},
       {"schemas", bj::array{bj::value(kNotesSchema)}},
       {"versions", bj::array{bj::object{{"manifest", kShopV2}, {"wat", shop_wat("two")}}}},
       {"content", bj::array{bj::object{{"tenant", "playground"},
                                        {"pages", bj::array{bj::object{{"path", "/about"},
                                                                       {"format", "markdown"},
                                                                       {"title", "About the sale"},
                                                                       {"body", "# Sale\n\nToday only."}}}}}}},
       {"flags", bj::array{bj::object{{"tenant", "playground"}, {"box", "shop"}, {"flag", "mode"}, {"value", "sale"}}}}});
  ASSERT_EQ(ok(added), "");
  ASSERT_EQ(added.at("members").as_array().size(), 4u) << json::dump(added);
  EXPECT_EQ(added.at("members").at(0), "schema:shopnotes1");  // kept in the order they apply
  EXPECT_EQ(added.at("members").at(3), "flag:playground/shop/mode");

  // The preview passes: the flag value "sale" is checked against v2, the version in the set.
  auto preview = tool("changeset_preview", {{"id", id}});
  ASSERT_EQ(ok(preview), "");
  EXPECT_TRUE(preview.at("ok").as_bool()) << json::dump(preview);
  EXPECT_NE(json::dump(preview.at("widens")).find("shop: "), std::string::npos) << json::dump(preview);

  auto applied = tool("changeset_apply", {{"id", id}});
  ASSERT_EQ(ok(applied), "");
  ASSERT_TRUE(applied.at("ok").as_bool()) << json::dump(applied);
  EXPECT_TRUE(applied.as_object().contains("change_ids")) << json::dump(applied);
  std::string v2 = live("shop");
  EXPECT_NE(v2, v1);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/mode").body, "sale");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/about").status, 200);
  // A change set applies once.
  EXPECT_TRUE(refused(tool("changeset_apply", {{"id", id}})));
  // The change log names each member's call with the change set.
  auto actions = tool("operator_actions", {});
  EXPECT_NE(json::dump(actions).find("\"changeset\":\"" + id + "\""), std::string::npos) << json::dump(actions);

  // A teammate edits the page afterwards: the rollback won't silently undo it.
  ASSERT_EQ(ok(tool("page_put", {{"tenant", "playground"}, {"path", "/about"}, {"format", "markdown"},
                                 {"body", "# Sale\n\nExtended!"}})),
            "");
  auto drift = tool("changeset_rollback", {{"id", id}});
  ASSERT_TRUE(refused(drift));
  EXPECT_NE(json::dump(drift).find("changed since"), std::string::npos) << json::dump(drift);
  EXPECT_EQ(live("shop"), v2);  // nothing was rolled back

  auto rolled = tool("changeset_rollback", {{"id", id}, {"force", true}});
  ASSERT_EQ(ok(rolled), "");
  EXPECT_TRUE(rolled.at("ok").as_bool()) << json::dump(rolled);
  EXPECT_EQ(live("shop"), v1);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/mode").body, "open");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/about").status, 404);
  // Schemas only grow: the set's schema stays stored.
  EXPECT_FALSE(refused(tool("schema_get", {{"name", "shopnotes1"}})));
  EXPECT_EQ(tool("changeset_show", {{"id", id}}).at("state"), "rolled_back");
  // Rolled back once is done.
  EXPECT_TRUE(refused(tool("changeset_rollback", {{"id", id}})));
}

TEST_F(ChangeSetTest, AFailurePartwayUndoesWhatWasApplied) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kShopV1, shop_wat("one"), &err)) << err;
  std::string v1 = live("shop");
  ASSERT_EQ(ok(tool("schema_put", {{"schema", kNotesSchema}})), "");
  auto v2 = host->boxes().store(kShopV2, shop_wat("two"), "", &err);
  ASSERT_TRUE(v2) << err;
  // other's /mode clashes with shop's: stored dark, it can't go live while shop serves /mode.
  auto other = tool("version_upload", {{"manifest", kOther}, {"wat", shop_wat("other")}, {"activate", false}});
  ASSERT_EQ(ok(other), "");
  std::string other_version(other.at("version").as_string());

  std::string id = create("two boxes");
  ASSERT_EQ(ok(tool("changeset_add", {{"id", id},
                                       {"versions", bj::array{bj::object{{"box", "shop"}, {"version", v2->version}},
                                                              bj::object{{"box", "other"}, {"version", other_version}}}}})),
            "");
  auto applied = tool("changeset_apply", {{"id", id}});
  ASSERT_EQ(ok(applied), "");
  EXPECT_FALSE(applied.at("ok").as_bool()) << json::dump(applied);
  EXPECT_EQ(applied.at("failed"), "version:other");
  EXPECT_EQ(applied.at("state"), "failed");
  // shop went to v2 first, then was put back.
  EXPECT_EQ(live("shop"), v1);
  EXPECT_EQ(live("other"), "");
  EXPECT_EQ(applied.at("undone").as_array().size(), 1u) << json::dump(applied);
  // A failed set can't be applied again or rolled back: it's done.
  EXPECT_TRUE(refused(tool("changeset_apply", {{"id", id}})));
  EXPECT_TRUE(refused(tool("changeset_rollback", {{"id", id}})));
}

TEST_F(ChangeSetTest, APreviewStoresNothing) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kShopV1, shop_wat("one"), &err)) << err;
  std::string v1 = live("shop");
  auto versions_before = host->boxes().versions("shop").size();

  std::string id = create("look only");
  ASSERT_EQ(ok(tool("changeset_add",
                    {{"id", id},
                     {"schemas", bj::array{bj::value(kNotesSchema)}},
                     {"versions", bj::array{bj::object{{"manifest", kShopV2}, {"wat", shop_wat("two")}}}},
                     {"content", bj::array{bj::object{{"tenant", "playground"},
                                                      {"files", bj::array{bj::object{{"path", "notes.txt"}, {"text", "hi"}}}}}}},
                     {"flags", bj::array{bj::object{{"tenant", "playground"}, {"box", "shop"}, {"flag", "mode"}, {"value", "closed"}}}}})),
            "");
  auto preview = tool("changeset_preview", {{"id", id}});
  ASSERT_EQ(ok(preview), "");
  EXPECT_TRUE(preview.at("ok").as_bool()) << json::dump(preview);
  EXPECT_EQ(preview.at("stored"), false);

  EXPECT_TRUE(refused(tool("schema_get", {{"name", "shopnotes1"}})));
  EXPECT_EQ(host->boxes().versions("shop").size(), versions_before);
  EXPECT_EQ(live("shop"), v1);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/notes.txt").status, 404);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/mode").body, "open");
  EXPECT_EQ(tool("changeset_show", {{"id", id}}).at("state"), "draft");

  // A flag value the box as it will be doesn't allow is a problem, and apply refuses on it.
  ASSERT_EQ(ok(tool("changeset_add", {{"id", id},
                                       {"flags", bj::array{bj::object{{"tenant", "playground"}, {"box", "shop"},
                                                                      {"flag", "mode"}, {"value", "maybe"}}}}})),
            "");
  auto bad = tool("changeset_apply", {{"id", id}});
  ASSERT_EQ(ok(bad), "");
  EXPECT_FALSE(bad.at("ok").as_bool());
  EXPECT_EQ(bad.at("applied"), false);
  EXPECT_NE(json::dump(bad.at("problems")).find("takes one of"), std::string::npos) << json::dump(bad);
  EXPECT_TRUE(refused(tool("schema_get", {{"name", "shopnotes1"}})));
  EXPECT_EQ(live("shop"), v1);

  // An apply pinned to an older draft is refused.
  auto stale = tool("changeset_apply", {{"id", id}, {"version", 1}});
  EXPECT_NE(json::dump(stale).find("changed since"), std::string::npos) << json::dump(stale);
}

// A schema's next version (041) can drop data and wait for a person: it isn't a change set member.
TEST_F(ChangeSetTest, ASchemasNextVersionIsNotAMember) {
  ASSERT_EQ(ok(tool("schema_put", {{"schema", kNotesSchema}})), "");
  std::string id = create("next version");
  const char* next = R"({"schema":"shopnotes2","follows":"shopnotes1","tables":{"notes":{"columns":{
    "id":{"type":"id","primary-key":true},"text":{"type":"string"}}}}})";
  auto r = tool("changeset_add", {{"id", id}, {"schemas", bj::array{bj::value(next)}}});
  EXPECT_TRUE(refused(r)) << json::dump(r);
  EXPECT_NE(json::dump(r).find("goes through schema_put"), std::string::npos);
}
