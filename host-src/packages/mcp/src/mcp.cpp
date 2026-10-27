#include "goober/mcp/mcp.h"

namespace goober::mcp {

namespace {
const char* kVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

bj::object error_body(const bj::value& id, int code, std::string message) {
  return bj::object{{"jsonrpc", "2.0"}, {"id", id}, {"error", bj::object{{"code", code}, {"message", message}}}};
}
}  // namespace

McpServer::McpServer(std::string name, std::string version, std::string instructions)
    : name_(std::move(name)), version_(std::move(version)), instructions_(std::move(instructions)) {}

void McpServer::add_tool(Tool tool) { tools_[tool.name] = std::move(tool); }

bj::value McpServer::dispatch(const bj::object& request, bool* is_notification) const {
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
      tools.push_back(bj::object{{"name", name}, {"description", tool.description}, {"inputSchema", tool.input_schema}});
    }
    result = bj::object{{"tools", std::move(tools)}};
  } else if (method == "tools/call") {
    auto n = params.find("name");
    if (n == params.end() || !n->value().is_string()) return error_body(id, -32602, "tools/call needs a name");
    auto tool = tools_.find(std::string(n->value().get_string()));
    if (tool == tools_.end()) return error_body(id, -32602, "unknown tool: " + std::string(n->value().get_string()));
    bj::object args;
    if (auto a = params.find("arguments"); a != params.end() && a->value().is_object()) args = a->value().get_object();
    ToolResult out;
    try {
      out = tool->second.run(args);
    } catch (const std::exception& e) {
      out = ToolResult::fail(std::string("tool failed: ") + e.what());
    }
    std::string text = out.value.is_string() ? std::string(out.value.get_string()) : goober::json::pretty(out.value);
    result = bj::object{{"content", bj::array{bj::object{{"type", "text"}, {"text", text}}}}, {"isError", out.is_error}};
    // An object result is also given as structured content, for clients that read it.
    if (!out.is_error && out.value.is_object()) result["structuredContent"] = out.value;
  } else {
    return error_body(id, -32601, "method not found: " + method);
  }
  return bj::object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

HttpReply McpServer::handle(std::string_view body) const {
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
      auto reply = dispatch(item.get_object(), &note);
      if (!note) out.push_back(std::move(reply));
    }
    if (out.empty()) return {202, ""};
    return {200, goober::json::dump(out)};
  }
  if (!parsed->is_object()) return {400, goober::json::dump(error_body(nullptr, -32600, "invalid request"))};
  auto reply = dispatch(parsed->get_object(), &note);
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
