#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "goober/json/json.h"

namespace goober::mcp {

namespace bj = boost::json;

struct ToolResult {
  bool is_error = false;
  bj::value value;  // returned as pretty JSON text
  static ToolResult ok(bj::value v) { return {false, std::move(v)}; }
  static ToolResult fail(std::string message) { return {true, bj::value(message)}; }
};

struct Tool {
  std::string name;
  std::string description;
  bj::object input_schema;  // a JSON Schema object
  std::function<ToolResult(const bj::object& arguments)> run;
};

struct HttpReply {
  int status = 200;
  std::string body;  // empty for 202
};

// An MCP server over streamable HTTP: JSON-RPC 2.0 in a POST, answered with
// application/json. Supports initialize, ping, tools/list, and tools/call.
class McpServer {
 public:
  McpServer(std::string name, std::string version, std::string instructions);
  void add_tool(Tool tool);
  HttpReply handle(std::string_view body) const;

 private:
  bj::value dispatch(const bj::object& request, bool* is_notification) const;
  std::string name_, version_, instructions_;
  std::map<std::string, Tool> tools_;
};

// Builds {"type":"object","properties":{...},"required":[...]}.
bj::object object_schema(std::initializer_list<std::pair<std::string, bj::value>> properties,
                         std::initializer_list<std::string> required);
bj::value string_property(std::string description);
bj::value integer_property(std::string description);
bj::value any_property(std::string description);
bj::value boolean_property(std::string description);
bj::value object_property(std::string description);
bj::value enum_property(std::string description, std::initializer_list<std::string> values);

}  // namespace goober::mcp
