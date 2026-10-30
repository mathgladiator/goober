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
  // Old argument names still accepted, each mapped to its current name: a renamed parameter
  // keeps working, so a caller's cached schema doesn't cost it a retry.
  std::map<std::string, std::string> aliases = {};
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
    // Fills in what the caller left out (like a tenant when only one is possible), before checks.
    std::function<void(const Tool& tool, bj::object& arguments)> prepare = {};
    std::function<std::optional<std::string>(const std::string& tool, const bj::object& arguments)> check;
    // A filter may also refuse the result as a whole: the call then answers that refusal.
    std::function<std::optional<std::string>(const std::string& tool, bj::value& result)> filter;
    // After the checks pass, may answer in the tool's place (an approval holding the call): the
    // tool isn't run, and the answer isn't filtered.
    std::function<std::optional<ToolResult>(const std::string& tool, const bj::object& arguments)> hold = {};
  };
  HttpReply handle(std::string_view body, const Allowed& allowed = {}) const;
  HttpReply handle(std::string_view body, const Policy& policy) const;
  // Every tool's name, sorted.
  std::vector<std::string> names() const;
  const Tool* tool(const std::string& name) const;
  // Each tool's schema version: the first 12 hex digits of its input schema's SHA-256, and its
  // aliases. Also in tools/list as _meta["goober/schema"].
  bj::array schemas() const;
  // Calls one tool directly, as tools/call would; nullopt for a tool that doesn't exist.
  // Aliases are renamed first; arguments the schema doesn't know are dropped, and named in an
  // object result's "ignored_arguments" so a caller learns its filter didn't apply.
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
