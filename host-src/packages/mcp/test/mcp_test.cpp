#include <gtest/gtest.h>

#include "goober/mcp/mcp.h"

using namespace goober;
namespace bj = boost::json;

namespace {
mcp::McpServer server() {
  mcp::McpServer s("goober-test", "0.1", "testing");
  s.add_tool({"echo", "Echoes text.", mcp::object_schema({{"text", mcp::string_property("what")}}, {"text"}),
              [](const bj::object& args) {
                auto it = args.find("text");
                if (it == args.end()) return mcp::ToolResult::fail("text is required");
                return mcp::ToolResult::ok(bj::object{{"echo", it->value()}});
              }});
  return s;
}
bj::value call(const mcp::McpServer& s, const std::string& body, int status = 200) {
  auto r = s.handle(body);
  EXPECT_EQ(r.status, status) << r.body;
  std::string err;
  return r.body.empty() ? bj::value() : *json::parse(r.body, &err);
}
}  // namespace

TEST(Mcp, Initialize) {
  auto s = server();
  auto v = call(s, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
  EXPECT_EQ(v.at("result").at("protocolVersion"), "2025-03-26");
  EXPECT_EQ(v.at("result").at("serverInfo").at("name"), "goober-test");
  v = call(s, R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"1999-01-01"}})");
  EXPECT_EQ(v.at("result").at("protocolVersion"), "2025-06-18");
}

TEST(Mcp, NotificationsGet202) {
  auto s = server();
  call(s, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", 202);
}

TEST(Mcp, ListAndCallTools) {
  auto s = server();
  auto v = call(s, R"({"jsonrpc":"2.0","id":"a","method":"tools/list"})");
  EXPECT_EQ(v.at("id"), "a");
  EXPECT_EQ(v.at("result").at("tools").as_array().size(), 1u);
  v = call(s, R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})");
  EXPECT_EQ(v.at("result").at("isError"), false);
  EXPECT_NE(std::string(v.at("result").at("content").at(0).at("text").as_string()).find("hi"), std::string::npos);
  v = call(s, R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{}}})");
  EXPECT_EQ(v.at("result").at("isError"), true);
}

TEST(Mcp, Errors) {
  auto s = server();
  auto v = call(s, "{nope", 400);
  EXPECT_EQ(v.at("error").at("code"), -32700);
  v = call(s, R"({"jsonrpc":"2.0","id":5,"method":"nope"})");
  EXPECT_EQ(v.at("error").at("code"), -32601);
  v = call(s, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"missing"}})");
  EXPECT_EQ(v.at("error").at("code"), -32602);
}

TEST(Mcp, Batches) {
  auto s = server();
  auto v = call(s, R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/x"}])");
  EXPECT_EQ(v.as_array().size(), 1u);
}

TEST(Mcp, AFilterHidesAndRefusesTools) {
  auto s = server();
  s.add_tool({"wipe", "Changes things.", mcp::object_schema({}, {}),
              [](const bj::object&) { return mcp::ToolResult::ok(bj::object{{"wiped", true}}); }});
  mcp::McpServer::Allowed reads = [](const std::string& t) { return t == "echo"; };
  std::string err;
  auto list = *json::parse(s.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", reads).body, &err);
  ASSERT_EQ(list.at("result").at("tools").as_array().size(), 1u);
  EXPECT_EQ(list.at("result").at("tools").at(0).at("name"), "echo");
  auto refused = *json::parse(
      s.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"wipe"}})", reads).body, &err);
  EXPECT_TRUE(refused.as_object().contains("error"));
  auto direct = s.call("echo", bj::object{{"text", "hi"}});
  ASSERT_TRUE(direct);
  EXPECT_EQ(direct->value.at("echo"), "hi");
  EXPECT_FALSE(s.call("nope", {}));
}

TEST(Mcp, AHoldAnswersInTheToolsPlaceUnfiltered) {
  goober::mcp::McpServer server("t", "1", "");
  int ran = 0;
  server.add_tool({"write", "w", goober::mcp::object_schema({}, {}), [&](const boost::json::object&) {
                     ++ran;
                     return goober::mcp::ToolResult::ok(boost::json::object{{"ran", true}});
                   }});
  goober::mcp::McpServer::Policy policy;
  policy.hold = [](const std::string& tool, const boost::json::object&) -> std::optional<goober::mcp::ToolResult> {
    return goober::mcp::ToolResult::ok(boost::json::object{{"pending_approval", 7}, {"tool", tool}});
  };
  policy.filter = [](const std::string&, boost::json::value&) { return std::optional<std::string>("filtered"); };
  auto reply = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"write","arguments":{}}})", policy);
  EXPECT_EQ(ran, 0);
  EXPECT_NE(reply.body.find("pending_approval"), std::string::npos) << reply.body;
  EXPECT_EQ(reply.body.find("filtered"), std::string::npos);
}

// With checked types, a policy's check and the tool read an integer or boolean argument the same
// way: normalized before either sees it, or refused (030 § 3).
TEST(Mcp, CheckedTypesNormalizeBeforeThePolicyAndTheTool) {
  mcp::McpServer s("goober-test", "0.1", "testing");
  s.set_checked_types(true);
  s.add_tool({"get", "Gets a thing.",
              mcp::object_schema({{"id", mcp::integer_property("id")}, {"dry_run", mcp::boolean_property("dry")},
                                  {"name", mcp::string_property("name")}},
                                 {"id"}),
              [](const bj::object& args) { return mcp::ToolResult::ok(bj::object{{"args", args}}); }});
  std::vector<std::string> checked;
  mcp::McpServer::Policy policy;
  policy.check = [&](const std::string&, const bj::object& args) -> std::optional<std::string> {
    checked.push_back(json::dump(args));
    if (auto id = mcp::integer_arg(args, "id"); id.ok && id.value == 7) return "id 7 is someone else's";
    return std::nullopt;
  };
  auto run = [&](const std::string& args) {
    auto r = s.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get","arguments":)" + args + "}}",
                      policy);
    std::string err;
    return json::parse(r.body, &err)->at("result");
  };
  // 7 in every whole-number shape meets the check as the integer 7.
  for (const char* id : {"7", "7.0", "\"7\""}) {
    auto r = run(std::string(R"({"id":)") + id + "}");
    EXPECT_TRUE(r.at("isError").as_bool()) << id;
    EXPECT_EQ(checked.back(), R"({"id":7})") << id;
  }
  // Not a whole number: refused before the check or the tool sees it.
  size_t seen = checked.size();
  for (const char* id : {"7.5", "true", "\"7x\"", "[7]", "{}", "18446744073709551615"}) {
    auto r = run(std::string(R"({"id":)") + id + "}");
    EXPECT_TRUE(r.at("isError").as_bool()) << id;
    EXPECT_NE(std::string(r.at("content").at(0).at("text").as_string()).find("id must be a whole number"), std::string::npos) << id;
  }
  EXPECT_EQ(checked.size(), seen);
  // Booleans: true/false, or their strings; strings and null left alone.
  auto ok = run(R"({"id":"8","dry_run":"true","name":5})");
  ASSERT_FALSE(ok.at("isError").as_bool()) << json::dump(ok);
  EXPECT_EQ(json::dump(ok.at("structuredContent").at("args")), R"({"id":8,"dry_run":true,"name":5})");
  EXPECT_TRUE(run(R"({"id":8,"dry_run":"yes"})").at("isError").as_bool());
  EXPECT_FALSE(run(R"({"id":8,"dry_run":null})").at("isError").as_bool());
  // A direct call is checked the same.
  EXPECT_TRUE(s.call("get", bj::object{{"id", 1.5}})->is_error);
  EXPECT_EQ(s.call("get", bj::object{{"id", 2.0}})->value.at("args").at("id").as_int64(), 2);
  // The shared reader.
  EXPECT_FALSE(mcp::integer_arg(bj::object{}, "id").present);
  EXPECT_FALSE(mcp::integer_arg(bj::object{{"id", nullptr}}, "id").present);
  EXPECT_EQ(mcp::integer_arg(bj::object{{"id", "-12"}}, "id").value, -12);
  EXPECT_FALSE(mcp::integer_arg(bj::object{{"id", 1e300}}, "id").ok);
}
