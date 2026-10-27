// The operator MCP endpoint (/goober-operations-mcp) and its tools
// (design/machine/005.FirstVersion.md §9).

#include <algorithm>

#include "goober/base/encoding.h"
#include "goober/base/names.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"
#include "goober/json/json.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

namespace {

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

int64_t int_arg(const bj::object& args, const char* key, int64_t fallback) {
  auto it = args.find(key);
  if (it == args.end()) return fallback;
  if (it->value().is_int64()) return it->value().get_int64();
  if (it->value().is_uint64()) return int64_t(it->value().get_uint64());
  return fallback;
}

bool looks_like_text(std::string_view data) {
  for (unsigned char c : data) {
    if (c == 0 || (c < 0x09) || (c > 0x0d && c < 0x20)) return false;
  }
  return true;
}

bj::array parse_lines(const std::vector<std::string>& lines) {
  bj::array out;
  for (const auto& line : lines) {
    std::string err;
    auto v = json::parse(line, &err);
    out.push_back(v ? *v : bj::value(line));
  }
  return out;
}

Response json_response(int status, std::string body) {
  Response res;
  res.status = status;
  if (!body.empty()) res.headers.emplace_back("content-type", "application/json");
  res.headers.emplace_back("cache-control", "no-store");
  res.body = std::move(body);
  return res;
}

}  // namespace

std::string& Host::current_operator() {
  thread_local std::string name;
  return name;
}

std::string Host::bearer_operator(const Request& req) const {
  auto cfg = config_.get();
  std::string auth = req.header("authorization");
  if (!cfg->operations_token.empty() && constant_time_equal(auth, "Bearer " + cfg->operations_token)) return "config";
  if (auth.starts_with("Bearer ")) {
    if (auto t = find_operator_token(cfg->storage, std::string_view(auth).substr(7))) return "token:" + t->name;
  }
  return "";
}

void Host::handle_operations(const Request& req, const Finish& finish) {
  std::string operator_name = bearer_operator(req);
  if (operator_name.empty()) {
    auto res = json_response(
        401, R"({"error":"a bearer token is required: mint one with goober-host token create, or set operations.token"})");
    res.headers.emplace_back("www-authenticate", "Bearer");
    return finish(std::move(res), R"({"why":"operations token"})");
  }
  if (req.method != "post") {
    auto res = json_response(405, "");
    res.headers.emplace_back("allow", "POST");
    return finish(std::move(res), "");
  }
  current_operator() = operator_name;
  auto reply = mcp_->handle(req.body);
  current_operator().clear();
  finish(json_response(reply.status, std::move(reply.body)),
         json::dump(bj::object{{"operations", "mcp"}, {"operator", operator_name}}));
}

void Host::register_tools() {
  using mcp::integer_property;
  using mcp::object_schema;
  using mcp::string_property;

  auto tenant_prop = string_property("A tenant name from the host config, like playground, or _host for host-wide "
                                     "content such as error-message/404.html.");

  mcp_->add_tool({"host_health", "Uptime, request count, storage path, boxes, and startup notes.", object_schema({}, {}),
                  [this](const bj::object&) {
                    bj::array notes;
                    for (const auto& n : startup_notes_) notes.push_back(bj::value(n));
                    auto cfg = config_.get();
                    return ToolResult::ok(bj::object{{"version", std::string(kVersion)},
                                                     {"uptime_ms", base::now_unix_ms() - started_ms_},
                                                     {"requests", requests_.load()},
                                                     {"queued_jobs", pool_->queued()},
                                                     {"storage", cfg->storage.string()},
                                                     {"boxes", boxes_->active()->boxes.size()},
                                                     {"startup_notes", std::move(notes)}});
                  }});

  mcp_->add_tool({"domain_list", "The domains this host answers, and the tenant each one serves.",
                  object_schema({}, {}), [this](const bj::object&) {
                    bj::array out;
                    for (const auto& [domain, entry] : config_.get()->domains) {
                      out.push_back(bj::object{{"domain", domain}, {"tenant", entry.tenant}, {"owner", entry.owner}});
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool(
      {"content_put",
       "Writes one static file for a tenant, persisted to disk through the host WAL. Give text, or base64 for binary. "
       "The path is relative, like index.html or css/site.css; / serves index.html.",
       object_schema({{"tenant", tenant_prop},
                      {"path", string_property("A relative path, like index.html.")},
                      {"text", string_property("The file's contents as text.")},
                      {"base64", string_property("The file's contents as base64, for binary files.")}},
                     {"tenant", "path"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto path = arg(args, "path");
         if (!tenant || !path) return ToolResult::fail("tenant and path are required");
         auto text = arg(args, "text");
         auto b64 = arg(args, "base64");
         if (bool(text) == bool(b64)) return ToolResult::fail("give exactly one of text or base64");
         std::string data;
         if (text) {
           data = *text;
         } else {
           auto decoded = base::base64_decode(*b64);
           if (!decoded) return ToolResult::fail("base64 is not valid");
           data = std::move(*decoded);
         }
         std::string err;
         if (!content_->put(*tenant, *path, data, &err)) return ToolResult::fail(err);
         auto f = content_->get(*tenant, *path);
         return ToolResult::ok(bj::object{{"tenant", *tenant},
                                          {"path", *path},
                                          {"bytes", data.size()},
                                          {"content_type", f ? f->content_type : ""},
                                          {"etag", f ? f->etag : ""}});
       }});

  mcp_->add_tool({"content_get", "Reads one static file. Text comes back as text, anything else as base64.",
                  object_schema({{"tenant", tenant_prop}, {"path", string_property("A relative path.")}},
                                {"tenant", "path"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto path = arg(args, "path");
                    if (!tenant || !path) return ToolResult::fail("tenant and path are required");
                    auto f = content_->get(*tenant, *path);
                    if (!f) return ToolResult::fail("no such file");
                    bj::object out{{"tenant", *tenant}, {"path", *path}, {"content_type", f->content_type},
                                   {"etag", f->etag},   {"bytes", f->data.size()}};
                    if (looks_like_text(f->data)) {
                      out["text"] = f->data;
                    } else {
                      out["base64"] = base::base64_encode(f->data);
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"content_list", "Lists a tenant's static files, optionally under a prefix.",
                  object_schema({{"tenant", tenant_prop}, {"prefix", string_property("Only paths starting with this.")}},
                                {"tenant"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!tenant) return ToolResult::fail("tenant is required");
                    bj::array out;
                    for (const auto& p : content_->list(*tenant, arg(args, "prefix").value_or(""))) {
                      out.push_back(bj::value(p));
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"content_delete", "Deletes one static file through the host WAL.",
                  object_schema({{"tenant", tenant_prop}, {"path", string_property("A relative path.")}},
                                {"tenant", "path"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto path = arg(args, "path");
                    if (!tenant || !path) return ToolResult::fail("tenant and path are required");
                    std::string err;
                    if (!content_->remove(*tenant, *path, &err)) return ToolResult::fail(err);
                    return ToolResult::ok(bj::object{{"deleted", *path}});
                  }});

  mcp_->add_tool(
      {"version_upload",
       "Uploads a box: a manifest (object or JSON text) and its module, as WAT text or base64 WASM. The host checks "
       "the manifest, the module's imports (only goober-abi@1), and one export goober_route_$name per route, then "
       "stores the version and makes it active (the v1 dev policy: no approval step). When the manifest names a "
       "types document (\"types\": \"x.openapi.json\"), pass that document as types: the OpenAPI components.schemas "
       "the routes' json:$Type bodies, responses, and tool inputs refer to, in goober's strict subset.",
       object_schema({{"manifest", mcp::any_property("The manifest, as a JSON object or as JSON text.")},
                      {"wat", string_property("The module as WebAssembly text.")},
                      {"wasm_base64", string_property("The module as base64 binary WASM.")},
                      {"types", mcp::any_property("The types document (OpenAPI 3.x), as a JSON object or text.")}},
                     {"manifest"}),
       [this](const bj::object& args) {
         auto it = args.find("manifest");
         if (it == args.end()) return ToolResult::fail("manifest is required");
         std::string manifest = it->value().is_string() ? std::string(it->value().get_string())
                                                         : json::pretty(it->value());
         auto wat = arg(args, "wat");
         auto b64 = arg(args, "wasm_base64");
         if (bool(wat) == bool(b64)) return ToolResult::fail("give exactly one of wat or wasm_base64");
         std::string module;
         if (wat) {
           module = *wat;
         } else {
           auto decoded = base::base64_decode(*b64);
           if (!decoded) return ToolResult::fail("wasm_base64 is not valid base64");
           module = std::move(*decoded);
         }
         std::string types;
         if (auto t = args.find("types"); t != args.end() && !t->value().is_null()) {
           types = t->value().is_string() ? std::string(t->value().get_string()) : json::pretty(t->value());
         }
         std::string err;
         auto result = boxes_->upload(manifest, module, types, &err);
         if (!result) return ToolResult::fail(err);
         bj::array warnings;
         for (const auto& w : result->warnings) warnings.push_back(bj::value(w));
         return ToolResult::ok(bj::object{{"version", result->version}, {"active", true}, {"warnings", warnings}});
       }});

  mcp_->add_tool({"version_list", "Lists a box's versions, and which is active.",
                  object_schema({{"box", string_property("The box name.")}}, {"box"}),
                  [this](const bj::object& args) {
                    auto box = arg(args, "box");
                    if (!box) return ToolResult::fail("box is required");
                    bj::array out;
                    for (const auto& v : boxes_->versions(*box)) {
                      out.push_back(bj::object{{"version", v.version}, {"active", v.active}});
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"version_get", "Returns a version's manifest.",
                  object_schema({{"box", string_property("The box name.")}, {"version", string_property("The version.")}},
                                {"box", "version"}),
                  [this](const bj::object& args) {
                    auto box = arg(args, "box");
                    auto version = arg(args, "version");
                    if (!box || !version) return ToolResult::fail("box and version are required");
                    auto text = boxes_->manifest_text(*box, *version);
                    if (!text) return ToolResult::fail("no such version");
                    std::string err;
                    auto v = json::parse(*text, &err);
                    return ToolResult::ok(bj::object{{"box", *box}, {"version", *version},
                                                     {"manifest", v ? *v : bj::value(*text)}});
                  }});

  mcp_->add_tool({"version_promote", "Makes an existing version active, which is how v1 rolls back.",
                  object_schema({{"box", string_property("The box name.")}, {"version", string_property("The version.")}},
                                {"box", "version"}),
                  [this](const bj::object& args) {
                    auto box = arg(args, "box");
                    auto version = arg(args, "version");
                    if (!box || !version) return ToolResult::fail("box and version are required");
                    std::string err;
                    if (!boxes_->promote(*box, *version, &err)) return ToolResult::fail(err);
                    return ToolResult::ok(bj::object{{"box", *box}, {"active", *version}});
                  }});

  mcp_->add_tool({"box_list", "Lists active boxes with their versions, domains, and routes.", object_schema({}, {}),
                  [this](const bj::object&) {
                    bj::array out;
                    for (const auto& [name, box] : boxes_->active()->boxes) {
                      bj::array routes;
                      for (const auto& r : box->manifest.routes) {
                        routes.push_back(bj::object{{"verb", r.verb}, {"path", r.pattern.source}, {"name", r.name}});
                      }
                      bj::array domains;
                      for (const auto& d : box->manifest.domains) domains.push_back(bj::value(d));
                      out.push_back(bj::object{{"box", name},
                                               {"version", box->version},
                                               {"mission", box->manifest.mission},
                                               {"domains", domains},
                                               {"routes", routes}});
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"box_logs", "The newest lines a box wrote with goober_log, plus host notes about failed calls.",
                  object_schema({{"box", string_property("The box name.")},
                                 {"limit", integer_property("How many lines, newest last. Default 50.")}},
                                {"box"}),
                  [this](const bj::object& args) {
                    auto box = arg(args, "box");
                    if (!box || !base::is_valid_name(*box)) return ToolResult::fail("a valid box name is required");
                    auto lines = base::tail_lines(config_.get()->storage / "logs" / "boxes" / (*box + ".log"),
                                                  size_t(std::clamp<int64_t>(int_arg(args, "limit", 50), 1, 1000)));
                    return ToolResult::ok(parse_lines(lines));
                  }});

  mcp_->add_tool({"access_log", "The newest lines of the access log.",
                  object_schema({{"limit", integer_property("How many lines, newest last. Default 50.")}}, {}),
                  [this](const bj::object& args) {
                    auto lines = base::tail_lines(config_.get()->storage / "logs" / "access.log",
                                                  size_t(std::clamp<int64_t>(int_arg(args, "limit", 50), 1, 1000)));
                    return ToolResult::ok(parse_lines(lines));
                  }});

  // Identity (013.Identity.md; design/machine/006.LocalIdentity.md). A realm is a tenant.
  auto realm_prop = string_property("A tenant name from the host config; its identity realm.");
  auto known_tenant = [this](const std::optional<std::string>& t) {
    if (!t) return false;
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == *t) return true;
    }
    return false;
  };
  mcp_->add_tool({"realm_get",
                  "A tenant realm's users, their roles and live sessions, and the newest identity audit records.",
                  object_schema({{"tenant", realm_prop}}, {"tenant"}),
                  [this, known_tenant](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    return ToolResult::ok(identity_->describe(*tenant, base::now_unix_ms()));
                  }});
  mcp_->add_tool(
      {"role_grant",
       "Grants a tenant role (like owner, admin, or admin/project_42) to an email in a tenant's realm. The user is "
       "created if the address is new, so the role waits for their first sign-in. Host roles come only from the "
       "host config.",
       object_schema({{"tenant", realm_prop},
                      {"email", string_property("The person's email address.")},
                      {"role", string_property("role or role/scope, each part [A-Za-z0-9_]+.")}},
                     {"tenant", "email", "role"}),
       [this, known_tenant](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto email = arg(args, "email");
         auto role = arg(args, "role");
         if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
         if (!email || !role) return ToolResult::fail("email and role are required");
         std::string why;
         if (!identity_->grant(*tenant, *email, *role, "operator", base::now_unix_ms(), &why)) {
           return ToolResult::fail(why);
         }
         return ToolResult::ok(bj::object{{"granted", *role}, {"to", *email}, {"tenant", *tenant}});
       }});
  mcp_->add_tool({"role_revoke", "Revokes a tenant role from an email in a tenant's realm.",
                  object_schema({{"tenant", realm_prop},
                                 {"email", string_property("The person's email address.")},
                                 {"role", string_property("The exact role string.")}},
                                {"tenant", "email", "role"}),
                  [this, known_tenant](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto email = arg(args, "email");
                    auto role = arg(args, "role");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    if (!email || !role) return ToolResult::fail("email and role are required");
                    std::string why;
                    if (!identity_->revoke(*tenant, *email, *role, "operator", base::now_unix_ms(), &why)) {
                      return ToolResult::fail(why);
                    }
                    return ToolResult::ok(bj::object{{"revoked", *role}, {"from", *email}, {"tenant", *tenant}});
                  }});
  mcp_->add_tool({"sessions_revoke", "Ends every session of a person in a tenant's realm, at their next request.",
                  object_schema({{"tenant", realm_prop}, {"email", string_property("The person's email address.")}},
                                {"tenant", "email"}),
                  [this, known_tenant](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto email = arg(args, "email");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    if (!email) return ToolResult::fail("email is required");
                    int n = identity_->revoke_sessions(*tenant, *email, "operator", base::now_unix_ms());
                    return ToolResult::ok(bj::object{{"ended", n}});
                  }});

  // OAuth clients (design/machine/009.OAuth.md §2). Clients in identity.clients exist in
  // every realm; these register a client in one realm.
  auto client_json = [](const identity::Client& c) {
    bj::array uris;
    for (const auto& u : c.redirect_uris) uris.push_back(bj::value(u));
    return bj::object{{"client_id", c.client_id},
                      {"name", c.name},
                      {"redirect_uris", std::move(uris)},
                      {"acts_as", c.person ? "person" : "agent"},
                      {"source", c.source}};
  };
  mcp_->add_tool({"client_list", "The OAuth clients registered in a tenant's realm: apps that act as the person, and agents.",
                  object_schema({{"tenant", realm_prop}}, {"tenant"}),
                  [this, known_tenant, client_json](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    bj::array out;
                    for (const auto& c : identity_->clients(*tenant)) out.push_back(client_json(c));
                    return ToolResult::ok(out);
                  }});
  mcp_->add_tool(
      {"client_put",
       "Registers or replaces an OAuth client in a tenant's realm. A person client (acts_as person) is the realm's own "
       "app and acts as the person, so only register one you trust like your own phone. Agents act for the person "
       "and reach only routes that allow agents.",
       object_schema({{"tenant", realm_prop},
                      {"client_id", string_property("1 to 64 of [A-Za-z0-9._-].")},
                      {"name", string_property("Shown on the consent screen.")},
                      {"redirect_uris", bj::value(bj::object{{"type", "array"},
                                                   {"items", bj::object{{"type", "string"}}},
                                                   {"description", "https URIs, http://127.0.0.1/... loopback (any "
                                                                   "port matches), or a private-use scheme like "
                                                                   "com.example.app:/callback."}})},
                      {"acts_as", mcp::enum_property("Whether the client acts as the person or as an agent.",
                                                {"person", "agent"})}},
                     {"tenant", "client_id", "name", "redirect_uris", "acts_as"}),
       [this, known_tenant, client_json](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
         identity::Client c;
         c.client_id = arg(args, "client_id").value_or("");
         c.name = arg(args, "name").value_or("");
         c.person = arg(args, "acts_as").value_or("") == "person";
         c.source = "operator";
         if (auto* u = args.if_contains("redirect_uris"); u && u->is_array()) {
           for (const auto& v : u->as_array()) {
             if (v.is_string()) c.redirect_uris.emplace_back(v.as_string());
           }
         }
         std::string why;
         if (!identity_->put_client(*tenant, c, base::now_unix_ms(), &why)) return ToolResult::fail(why);
         return ToolResult::ok(client_json(*identity_->client(*tenant, c.client_id)));
       }});
  mcp_->add_tool({"client_remove", "Removes an OAuth client from a tenant's realm, ending every token it holds.",
                  object_schema({{"tenant", realm_prop}, {"client_id", string_property("The client to remove.")}},
                                {"tenant", "client_id"}),
                  [this, known_tenant](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto id = arg(args, "client_id");
                    if (!known_tenant(tenant)) return ToolResult::fail("not a tenant in the host config");
                    if (!id || !identity_->remove_client(*tenant, *id, base::now_unix_ms())) {
                      return ToolResult::fail("no such client");
                    }
                    return ToolResult::ok(bj::object{{"removed", *id}});
                  }});
}

}  // namespace goober::host
