// /mcp on every domain (050.AgentsMCP.md § What the host does for free): MCP over
// streamable HTTP for agents and people holding a bearer token from this realm. The tools
// are the box tools (routes.mcp) active on the domain, filtered by the caller's roles, so
// an agent never learns a tool exists unless it may call it. See design/machine/012.Agents.md.

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/mcp/mcp.h"

namespace goober::host {
namespace bj = boost::json;
using server::http::Request;
using server::http::Response;

namespace {

// The newest bytes of a value, for the audit: enough to read, never unbounded.
std::string clip(const std::string& s, size_t n = 2000) { return s.size() <= n ? s : s.substr(0, n) + "..."; }

}  // namespace

void Host::handle_tenant_mcp(const Request& req, const Finish& finish, const std::string& domain,
                             const std::string& tenant) {
  std::string request_id = req.header("goober-request-id");
  auto who = signed_in(req, tenant);
  bool bearer = base::to_lower(req.header("authorization")).starts_with("bearer ");
  if (!who || !bearer) {
    // MCP authorization: point the client at this realm's protected-resource metadata,
    // which names the authorization server it signs in with (RFC 9728).
    Response res;
    res.status = 401;
    res.headers.emplace_back("content-type", "application/json");
    res.headers.emplace_back("cache-control", "no-store");
    res.headers.emplace_back("www-authenticate", std::string("Bearer") + (bearer ? " error=\"invalid_token\"," : "") +
                                                     " resource_metadata=\"" + origin_of(req) +
                                                     "/.well-known/oauth-protected-resource\"");
    res.body = R"({"error":"sign in with OAuth: a bearer token from this domain's realm"})";
    return finish(std::move(res), R"({"mcp":"unauthorized"})");
  }
  if (req.method != "post") {
    Response res;
    res.status = 405;
    res.headers.emplace_back("allow", "POST");
    return finish(std::move(res), R"({"mcp":"method"})");
  }
  if (who->host) return finish(error_page(tenant, 403, request_id), R"({"mcp":"host identities have no tools here"})");

  // One call per POST: MCP dropped JSON-RPC batches, and a batch would also slip past the
  // rate limit and the one-line-per-call audit.
  if (auto first = req.body.find_first_not_of(" \t\r\n"); first != std::string::npos && req.body[first] == '[') {
    Response res;
    res.status = 400;
    res.headers.emplace_back("content-type", "application/json");
    res.body = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"batches aren't accepted; send one request per POST"}})";
    return finish(std::move(res), R"({"mcp":"batch refused"})");
  }
  auto active = boxes_->active();
  std::optional<manifest::Caller> caller = manifest::Caller{std::to_string(who->user.user_id), false, who->roles};
  size_t allowed = 0;
  for (const auto& [name, ref] : active->tools_for(domain)) {
    allowed += manifest::check_access(*ref.route, &*caller, {}, tenant) == manifest::Access::kAllowed;
  }
  // An agent that finds nothing here was most often meant for the operator endpoint: say so,
  // rather than leave an empty list to be read as a broken connection.
  std::string none = allowed > 0 ? ""
                                 : " This site offers this person no tools here (its boxes declare none, or none their "
                                   "roles allow). To build and operate the site (pages, templates, uploads, boxes), "
                                   "connect to " + origin_of(req) + std::string(kOperationsMcpPath) +
                                   " with an operator token from `goober-host token create`.";
  mcp::McpServer server(
      "goober", std::string(kVersion),
      "Tools from " + domain + ", acting for " + who->user.email + (who->agent ? " as an agent" : "") +
          ". Each tool says what it does; inputs are checked before anything runs." + none);
  std::string audit_tool;
  int audit_status = 0;
  std::string audit_input, audit_result;
  for (const auto& [name, ref] : active->tools_for(domain)) {
    // An agent only discovers what the person behind it may call (050).
    if (manifest::check_access(*ref.route, &*caller, {}, tenant) != manifest::Access::kAllowed) continue;
    bj::object schema{{"type", "object"}, {"properties", bj::object{}}, {"additionalProperties", false}};
    std::string input_type = types::json_type_name(ref.route->input);
    if (!input_type.empty() && ref.box->types) schema = ref.box->types->json_schema(input_type).as_object();
    server.add_tool(mcp::Tool{
        name, ref.route->description, schema, [&, ref, input_type, name](const bj::object& args) -> mcp::ToolResult {
          audit_tool = name;
          audit_input = json::dump(args);
          bj::value input = args;
          if (!input_type.empty()) {
            std::string err;
            auto checked = ref.box->types->check(input_type, args, &err);
            if (!checked) {
              audit_status = 400;
              audit_result = err;
              return mcp::ToolResult::fail(input_type + " " + err);
            }
            input = std::move(*checked);
          } else if (!args.empty()) {
            audit_status = 400;
            return mcp::ToolResult::fail("this tool takes no input");
          }
          bj::object doc{{"request_id", request_id}, {"method", "tool"},     {"path", "mcp:" + name},
                         {"params", bj::object{}},   {"query", bj::object{}}, {"headers", bj::object{}},
                         {"body", json::dump(input)}, {"domain", domain},     {"tenant", tenant},
                         {"user", user_json(who)}};
          auto call = call_box(ref, doc, tenant, request_id, who);
          const auto& r = call.result;
          audit_status = r.outcome == runtime::wasm::CallResult::Outcome::kOk ? r.status : 500;
          audit_result = r.body;
          if (r.outcome != runtime::wasm::CallResult::Outcome::kOk || r.status >= 500) {
            // A host failure isn't the model's to reason about (050 § The result vocabulary).
            return mcp::ToolResult::fail("the tool failed; request " + request_id);
          }
          std::string err;
          auto parsed = r.body.empty() ? std::optional<bj::value>(bj::object{}) : json::parse(r.body, &err);
          if (r.status >= 400) {
            std::string message = r.body;
            if (parsed && parsed->is_object() && parsed->as_object().contains("error") &&
                parsed->as_object().at("error").is_string()) {
              message = std::string(parsed->as_object().at("error").as_string());
            }
            return mcp::ToolResult::fail(message.empty() ? "refused (" + std::to_string(r.status) + ")" : message);
          }
          if (parsed) return mcp::ToolResult::ok(std::move(*parsed));
          return mcp::ToolResult::ok(bj::value(r.body));
        }});
  }
  auto reply = server.handle(req.body);
  Response res;
  res.status = reply.status;
  res.body = reply.body;
  res.headers.emplace_back("cache-control", "no-store");
  if (!reply.body.empty()) res.headers.emplace_back("content-type", "application/json");
  bj::object extra{{"mcp", audit_tool.empty() ? "rpc" : "tool"}, {"user_id", std::to_string(who->user.user_id)}};
  if (!audit_tool.empty()) {
    extra["tool"] = audit_tool;
    extra["tool_status"] = audit_status;
    // Every tool call is written down: the agent, the person, the input, and the result (050).
    base::append_line(config_.get()->storage / "logs" / "agents.log",
                      json::dump(bj::object{{"at", base::format_utc_ms(base::now_unix_ms())},
                                            {"request_id", request_id},
                                            {"tenant", tenant},
                                            {"user_id", std::to_string(who->user.user_id)},
                                            {"client_id", who->client_id},
                                            {"agent", who->agent},
                                            {"tool", audit_tool},
                                            {"status", audit_status},
                                            {"input", clip(audit_input)},
                                            {"result", clip(audit_result)}}));
  }
  if (who->agent) extra["client_id"] = who->client_id;
  finish(std::move(res), json::dump(extra));
}

}  // namespace goober::host
