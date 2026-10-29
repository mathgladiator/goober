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
