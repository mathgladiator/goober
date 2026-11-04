#include "goober/mcp/mcp.h"

#include <cmath>
#include <cstdint>

#include "goober/base/hash.h"

namespace goober::mcp {

namespace {
const char* kVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

std::string schema_hash(const bj::object& schema) {
  return goober::base::sha256_hex(goober::json::dump(schema)).substr(0, 12);
}

bj::object error_body(const bj::value& id, int code, std::string message) {
  return bj::object{{"jsonrpc", "2.0"}, {"id", id}, {"error", bj::object{{"code", code}, {"message", message}}}};
}
}  // namespace

McpServer::McpServer(std::string name, std::string version, std::string instructions)
    : name_(std::move(name)), version_(std::move(version)), instructions_(std::move(instructions)) {}

void McpServer::add_tool(Tool tool) { tools_[tool.name] = std::move(tool); }

bj::value McpServer::dispatch(const bj::object& request, bool* is_notification, const Policy& policy) const {
  const Allowed& allowed = policy.allowed;
  auto id_it = request.find("id");
  *is_notification = id_it == request.end();
  bj::value id = *is_notification ? bj::value(nullptr) : id_it->value();
  auto method_it = request.find("method");
  if (method_it == request.end() || !method_it->value().is_string()) {
    *is_notification = false;
    return error_body(id, -32600, "invalid request: missing method");
  }
  std::string method(method_it->value().get_string());
  bj::object params;
  if (auto p = request.find("params"); p != request.end() && p->value().is_object()) params = p->value().get_object();

  if (*is_notification) return nullptr;  // notifications/initialized and friends need no answer

  bj::object result;
  if (method == "initialize") {
    std::string wanted;
    if (auto v = params.find("protocolVersion"); v != params.end() && v->value().is_string()) {
      wanted = std::string(v->value().get_string());
    }
    std::string chosen = kVersions[0];
    for (auto v : kVersions) {
      if (wanted == v) chosen = v;
    }
    result = bj::object{{"protocolVersion", chosen},
                        {"capabilities", bj::object{{"tools", bj::object{{"listChanged", false}}}}},
                        {"serverInfo", bj::object{{"name", name_}, {"version", version_}}},
                        {"instructions", instructions_}};
  } else if (method == "ping") {
    result = bj::object{};
  } else if (method == "tools/list") {
    bj::array tools;
    for (const auto& [name, tool] : tools_) {
      if (allowed && !allowed(name)) continue;
      bj::object t{{"name", name}, {"description", tool.description}, {"inputSchema", tool.input_schema}};
      bj::object meta{{"goober/schema", schema_hash(tool.input_schema)}};
      if (auto m = meta_.find(name); m != meta_.end()) {
        for (const auto& kv : m->second) meta[kv.key()] = kv.value();
      }
      t["_meta"] = std::move(meta);
      tools.push_back(std::move(t));
    }
    result = bj::object{{"tools", std::move(tools)}};
  } else if (method == "tools/call") {
    auto n = params.find("name");
    if (n == params.end() || !n->value().is_string()) return error_body(id, -32602, "tools/call needs a name");
    std::string name(n->value().get_string());
    bj::object args;
    if (auto a = params.find("arguments"); a != params.end() && a->value().is_object()) args = a->value().get_object();
    // Aliases and argument types first, so the policy's checks read what the tool will.
    std::optional<std::string> wrong;
    if (tools_.count(name)) wrong = admit(tools_.at(name), args);
    if (policy.prepare && tools_.count(name)) policy.prepare(tools_.at(name), args);
    if (allowed && tools_.count(name) && !allowed(name)) {
      std::string why = "its access is read-only";
      if (policy.check) {
        if (auto refused = policy.check(name, args)) why = *refused;
      }
      if (policy.after) policy.after(name, args, ToolResult::fail(why), false);
      return error_body(id, -32602, "tool " + name + " is not allowed for this caller: " + why);
    }
    if (!tools_.count(name)) return error_body(id, -32602, "unknown tool: " + name);
    std::optional<ToolResult> called;
    if (wrong) called = ToolResult::fail(*wrong);
    if (!called && policy.check) {
      if (auto refused = policy.check(name, args)) called = ToolResult::fail(*refused);
    }
    bool held = false;
    if (!called && policy.hold) {
      called = policy.hold(name, args);
      held = called.has_value();
    }
    if (!called) called = call(name, args);
    ToolResult out = std::move(*called);
    if (!out.is_error && policy.filter && !held) {
      if (auto refused = policy.filter(name, out.value)) out = ToolResult::fail(*refused);
    }
    if (policy.after) policy.after(name, args, out, held);
    std::string text = out.value.is_string() ? std::string(out.value.get_string()) : goober::json::pretty(out.value);
    result = bj::object{{"content", bj::array{bj::object{{"type", "text"}, {"text", text}}}}, {"isError", out.is_error}};
    // An object result is also given as structured content, for clients that read it.
    if (!out.is_error && out.value.is_object()) result["structuredContent"] = out.value;
  } else {
    return error_body(id, -32601, "method not found: " + method);
  }
  return bj::object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

std::optional<ToolResult> McpServer::call(const std::string& name, const bj::object& in) const {
  auto tool = tools_.find(name);
  if (tool == tools_.end()) return std::nullopt;
  bj::object args = in;
  if (auto wrong = admit(tool->second, args)) return ToolResult::fail(*wrong);
  bj::array ignored;
  if (auto* props = tool->second.input_schema.if_contains("properties"); props && props->is_object()) {
    for (auto it = args.begin(); it != args.end();) {
      if (!props->as_object().contains(it->key())) {
        ignored.push_back(bj::value(it->key()));
        it = args.erase(it);
      } else {
        ++it;
      }
    }
  }
  try {
    auto out = tool->second.run(args);
    if (!ignored.empty()) {
      if (out.value.is_object()) {
        out.value.as_object()["ignored_arguments"] = std::move(ignored);
      } else if (out.is_error && out.value.is_string()) {
        out.value = std::string(out.value.as_string()) + " (ignored unknown arguments: " + goober::json::dump(ignored) + ")";
      }
    }
    return out;
  } catch (const std::exception& e) {
    return ToolResult::fail(std::string("tool failed: ") + e.what());
  }
}

std::optional<std::string> McpServer::admit(const Tool& tool, bj::object& args) const {
  for (const auto& [old_name, current] : tool.aliases) {
    if (auto* v = args.if_contains(old_name)) {
      if (!args.contains(current)) args[current] = *v;
      args.erase(old_name);
    }
  }
  if (!checked_types_) return std::nullopt;
  const auto* props = tool.input_schema.if_contains("properties");
  if (!props || !props->is_object()) return std::nullopt;
  for (auto& kv : args) {
    const auto* p = props->as_object().if_contains(kv.key());
    if (!p || !p->is_object() || kv.value().is_null()) continue;
    const auto* t = p->as_object().if_contains("type");
    if (!t || !t->is_string()) continue;
    std::string_view type = t->as_string();
    bj::value& v = kv.value();
    if (type == "integer") {
      auto n = integer_value(v);
      if (!n.ok) return "argument " + std::string(kv.key()) + " must be a whole number";
      v = n.value;
    } else if (type == "boolean") {
      if (v.is_bool()) continue;
      if (v.is_string() && (v.as_string() == "true" || v.as_string() == "false")) {
        v = v.as_string() == "true";
        continue;
      }
      return "argument " + std::string(kv.key()) + " must be true or false";
    }
  }
  return std::nullopt;
}

IntArg integer_value(const bj::value& v) {
  IntArg out;
  if (v.is_null()) return out;
  out.present = true;
  if (v.is_int64()) {
    out.ok = true;
    out.value = v.as_int64();
  } else if (v.is_uint64()) {
    if (v.as_uint64() <= uint64_t(INT64_MAX)) out.ok = true, out.value = int64_t(v.as_uint64());
  } else if (v.is_double()) {
    double d = v.as_double();
    // Whole and well inside int64 (2^62), so the conversion is exact and defined.
    if (std::isfinite(d) && std::floor(d) == d && d > -4.6e18 && d < 4.6e18) out.ok = true, out.value = int64_t(d);
  } else if (v.is_string()) {
    std::string_view s = v.as_string();
    bool neg = !s.empty() && s.front() == '-';
    if (neg) s.remove_prefix(1);
    if (!s.empty() && s.size() <= 18 && s.find_first_not_of("0123456789") == std::string_view::npos) {
      int64_t n = 0;
      for (char c : s) n = n * 10 + (c - '0');
      out.ok = true;
      out.value = neg ? -n : n;
    }
  }
  return out;
}

IntArg integer_arg(const bj::object& args, std::string_view key) {
  const auto* v = args.if_contains(key);
  return v ? integer_value(*v) : IntArg{};
}

const Tool* McpServer::tool(const std::string& name) const {
  auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : &it->second;
}

bj::array McpServer::schemas() const {
  bj::array out;
  for (const auto& [name, t] : tools_) {
    bj::object aliases;
    for (const auto& [a, b] : t.aliases) aliases[a] = b;
    out.push_back(bj::object{{"tool", name}, {"schema", schema_hash(t.input_schema)}, {"aliases", std::move(aliases)}});
  }
  return out;
}

std::vector<std::string> McpServer::names() const {
  std::vector<std::string> out;
  for (const auto& [name, tool] : tools_) out.push_back(name);
  return out;
}

HttpReply McpServer::handle(std::string_view body, const Allowed& allowed) const {
  return handle(body, Policy{allowed, {}, {}});
}

HttpReply McpServer::handle(std::string_view body, const Policy& policy) const {
  std::string err;
  auto parsed = goober::json::parse(body, &err);
  if (!parsed) return {400, goober::json::dump(error_body(nullptr, -32700, err))};
  bool note = false;
  if (parsed->is_array()) {
    bj::array out;
    for (const auto& item : parsed->get_array()) {
      if (!item.is_object()) {
        out.push_back(error_body(nullptr, -32600, "invalid request"));
        continue;
      }
      auto reply = dispatch(item.get_object(), &note, policy);
      if (!note) out.push_back(std::move(reply));
    }
    if (out.empty()) return {202, ""};
    return {200, goober::json::dump(out)};
  }
  if (!parsed->is_object()) return {400, goober::json::dump(error_body(nullptr, -32600, "invalid request"))};
  auto reply = dispatch(parsed->get_object(), &note, policy);
  if (note) return {202, ""};
  return {200, goober::json::dump(reply)};
}

bj::object object_schema(std::initializer_list<std::pair<std::string, bj::value>> properties,
                         std::initializer_list<std::string> required) {
  bj::object props;
  for (const auto& [k, v] : properties) props[k] = v;
  bj::array req;
  for (const auto& r : required) req.push_back(bj::value(r));
  return bj::object{{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(req)}};
}

bj::value string_property(std::string description) {
  return bj::object{{"type", "string"}, {"description", description}};
}
bj::value integer_property(std::string description) {
  return bj::object{{"type", "integer"}, {"description", description}};
}
bj::value any_property(std::string description) { return bj::object{{"description", description}}; }

bj::value boolean_property(std::string description) {
  return bj::object{{"type", "boolean"}, {"description", description}};
}

bj::value object_property(std::string description) {
  return bj::object{{"type", "object"}, {"description", description}};
}

bj::value enum_property(std::string description, std::initializer_list<std::string> values) {
  bj::array e;
  for (const auto& v : values) e.push_back(bj::value(v));
  return bj::object{{"type", "string"}, {"enum", std::move(e)}, {"description", description}};
}

}  // namespace goober::mcp
