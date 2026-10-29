#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
  // A tool's _meta in tools/list (MCP 2025-06-18), like which box version answers it.
  void set_tool_meta(const std::string& name, bj::object meta) { meta_[name] = std::move(meta); }
  // Which tools a caller may see and call; an empty filter allows every tool.
  using Allowed = std::function<bool(const std::string& tool)>;
  // A caller's whole policy: which tools it sees, a check of each call's arguments (a refusal
  // is the tool's error), and a filter over each result it gets back.
  struct Policy {
    Allowed allowed;
    std::function<std::optional<std::string>(const std::string& tool, const bj::object& arguments)> check;
    // A filter may also refuse the result as a whole: the call then answers that refusal.
    std::function<std::optional<std::string>(const std::string& tool, bj::value& result)> filter;
  };
  HttpReply handle(std::string_view body, const Allowed& allowed = {}) const;
  HttpReply handle(std::string_view body, const Policy& policy) const;
  // Every tool's name, sorted.
  std::vector<std::string> names() const;
  // Calls one tool directly, as tools/call would; nullopt for a tool that doesn't exist.
  std::optional<ToolResult> call(const std::string& tool, const bj::object& arguments) const;

 private:
  bj::value dispatch(const bj::object& request, bool* is_notification, const Policy& policy) const;
  std::string name_, version_, instructions_;
  std::map<std::string, Tool> tools_;
  std::map<std::string, bj::object> meta_;
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
