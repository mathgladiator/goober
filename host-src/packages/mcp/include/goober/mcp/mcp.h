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
    // After every tools/call, refused ones included, batched or not: for an audit.
    std::function<void(const std::string& tool, const bj::object& arguments, const ToolResult& result, bool held)> after = {};
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
  // With checked types on, each top-level argument the schema types "integer" or "boolean" is
  // checked before anything sees it (the policy's checks, then the tool), and put in one form:
  // a whole number (an integer, a whole double like 3.0, or a string of digits) becomes an
  // integer; true/false (or "true"/"false") a boolean; anything else is refused, naming the
  // argument. A null is left as it is, which every reader takes as absent. So a scope rule and
  // the tool it guards can never read one argument two ways.
  void set_checked_types(bool on) { checked_types_ = on; }
  // Renames aliases and, with checked types on, checks and normalizes the arguments as above:
  // the refusal, or nullopt with `arguments` ready for the policy and the tool.
  std::optional<std::string> admit(const Tool& tool, bj::object& arguments) const;

 private:
  bj::value dispatch(const bj::object& request, bool* is_notification, const Policy& policy) const;
  std::string name_, version_, instructions_;
  std::map<std::string, Tool> tools_;
  std::map<std::string, bj::object> meta_;
  bool checked_types_ = false;
};

// An integer argument, read one way everywhere (a scope rule and its tool alike): absent (or
// null); a whole number (an integer, a whole double like 3.0, or a string of at most 18 digits
// with an optional leading "-"); or present but not one (`ok` false), which a caller refuses.
struct IntArg {
  bool present = false;
  bool ok = false;
  int64_t value = 0;
};
IntArg integer_arg(const bj::object& arguments, std::string_view key);
IntArg integer_value(const bj::value& v);

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
