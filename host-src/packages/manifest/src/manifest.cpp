#include "goober/manifest/manifest.h"

#include "goober/router/table.h"

#include <algorithm>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/names.h"
#include "goober/json/json.h"
#include "goober/render/mustache.h"

namespace goober::manifest {
namespace bj = boost::json;
using goober::json::get_int;
using goober::json::get_object;
using goober::json::get_string;

bool Manifest::applies_to(std::string_view domain) const {
  for (const auto& d : domains) {
    if (d == "*" || d == domain) return true;
  }
  return false;
}

const std::vector<std::string>& reserved_prefixes() {
  static const std::vector<std::string> kPrefixes = {
      "/.well-known", "/oauth",    "/identity",        "/mcp",          "/goober",
      "/goober-operations", "/goober-operations-mcp", "/internal", "/error-message", "/attachment"};
  return kPrefixes;
}

namespace {

// Fields from 150.DeepManifest.md that v1 accepts with a warning.
const std::set<std::string> kLaterTopLevel = {"pins",    "policy",   "deployment",
                                              "external", "keys", "static",   "headers",
                                              "rewrites", "counters", "working-set", "content-security"};
const std::set<std::string> kLaterBehavior = {"mode", "owned-by", "lease-ms", "boundary", "lifecycle", "budget",
                                              "instances"};
const std::set<std::string> kLaterRoute = {"headers", "callers", "signal", "timeout-ms", "grantable", "revocable",
                                           "queue",   "shadow-calls"};
const std::set<std::string> kResponses = {"json", "text", "html", "markdown", "empty"};

bool is_header_name(std::string_view h) {
  if (h.empty() || h.size() > 64) return false;
  for (char c : h) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

// The request parts of 037: body, max-body-bytes, query, request-headers, and response.
bool parse_request_parts(const bj::object& def, const std::string& verb, const std::string& where, HttpRoute* route,
                         std::string* error) {
  std::string err;
  if (auto b = get_string(def, "body", &err)) {
    if (*b != "text" && *b != "binary" && types::json_type_name(*b).empty()) {
      return *error = where + ".body must be json:$Type, text, or binary", false;
    }
    if (verb == "get" || verb == "delete") return *error = where + ": a " + verb + " carries no body", false;
    route->body = *b;
  }
  if (!err.empty()) return *error = where + ".body: " + err, false;
  if (auto n = get_int(def, "max-body-bytes", &err)) {
    if (*n < 1 || *n > (int64_t(1) << 30)) return *error = where + ".max-body-bytes must be 1 to 1073741824", false;
    route->max_body_bytes = *n;
  }
  if (!err.empty()) return *error = where + ".max-body-bytes: " + err, false;
  if (auto* q = def.if_contains("query")) {
    if (!q->is_object()) return *error = where + ".query must be an object of parameter schemas", false;
    route->query_declared = true;
    for (const auto& kv : q->as_object()) {
      std::string name(kv.key());
      bool ok = !name.empty() && name.size() <= 64 && !(name[0] >= '0' && name[0] <= '9') && name[0] != '!';
      for (char c : name) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
      if (!ok) return *error = where + ".query: "" + name + "" isn't a valid parameter name", false;
      auto schema = types::parse_schema(kv.value(), where + ".query." + name, true, error);
      if (!schema) return false;
      route->query.emplace_back(name, *schema);
    }
  }
  if (auto* h = def.if_contains("request-headers")) {
    if (!h->is_array()) return *error = where + ".request-headers must be a list of header names", false;
    route->headers_declared = true;
    for (const auto& x : h->as_array()) {
      std::string name = x.is_string() ? base::to_lower(std::string(x.as_string())) : "";
      if (!is_header_name(name)) return *error = where + ".request-headers holds a bad header name", false;
      // Credentials belong to the host, never to a box (037 § Everything a request can carry).
      if (name == "authorization" || name == "cookie" || name.starts_with("goober-")) {
        return *error = where + ".request-headers can't name " + name + ": the host keeps it", false;
      }
      route->request_headers.push_back(name);
    }
  }
  if (auto r = get_string(def, "response", &err)) {
    if (!kResponses.count(*r) && types::json_type_name(*r).empty()) {
      return *error = where + ".response must be json, json:$Type, text, html, markdown, or empty", false;
    }
    route->response = *r;
  }
  if (!err.empty()) return *error = where + ".response: " + err, false;
  return true;
}

bool parse_requirements_value(const bj::value& v, const router::RoutePattern& p, std::vector<std::vector<std::string>>* out,
                              std::string* why);

// routes.mcp.$name.tool (050.AgentsMCP.md § Tools are routes). Resources come later.
bool parse_tools(const bj::value& mcp, std::set<std::string>* names, Manifest* m, std::string* error) {
  if (!mcp.is_object()) return *error = "routes.mcp must be an object", false;
  std::string err;
  for (const auto& kv : mcp.as_object()) {
    std::string key(kv.key());
    std::string where = "routes.mcp." + key;
    if (!kv.value().is_object()) return *error = where + " must be an object", false;
    for (const auto& verb : kv.value().as_object()) {
      std::string v(verb.key());
      if (v == "resource") {
        m->warnings.push_back(where + ".resource is accepted but not implemented in v1");
        continue;
      }
      if (v != "tool") return *error = where + ": unknown verb (tool, resource)", false;
      std::string at = where + ".tool";
      if (!verb.value().is_object()) return *error = at + " must be an object", false;
      const auto& def = verb.value().as_object();
      for (const auto& f : def) {
        std::string k(f.key());
        if (k == "name" || k == "description" || k == "input" || k == "response" || k == "requirements") continue;
        if (k == "public") return *error = at + ": MCP routes can't be public; an agent always acts for somebody", false;
        if (k == "grantable" || k == "revocable" || k == "queue") {
          m->warnings.push_back(at + "." + k + " is accepted but not implemented in v1");
          continue;
        }
        return *error = at + ": unknown field \"" + k + "\"", false;
      }
      HttpRoute tool;
      tool.verb = "tool";
      tool.name = key;
      if (auto n = get_string(def, "name", &err); n && *n != key) {
        return *error = at + ".name must be the tool's key, " + key, false;
      }
      if (!base::is_valid_name(key)) return *error = where + ": tool name must match [a-z][a-z0-9_]* (at most 63)", false;
      if (!names->insert(key).second) return *error = "route name \"" + key + "\" is used twice", false;
      auto d = get_string(def, "description", &err);
      if (!d || d->empty() || d->size() > 2000) {
        return *error = at + ".description is required (at most 2000 bytes): a model reads it and acts on it", false;
      }
      tool.description = *d;
      if (auto in = get_string(def, "input", &err)) {
        if (types::json_type_name(*in).empty()) return *error = at + ".input must be json:$Type", false;
        tool.input = *in;
      }
      if (auto r = get_string(def, "response", &err)) {
        if (*r != "json" && *r != "text" && types::json_type_name(*r).empty()) {
          return *error = at + ".response must be json, json:$Type, or text", false;
        }
        tool.response = *r;
      }
      if (!err.empty()) return *error = at + ": " + err, false;
      auto* req = def.if_contains("requirements");
      if (!req) return *error = at + ": needs requirements; a tool with none is denied to everyone", false;
      std::string why;
      if (!parse_requirements_value(*req, router::RoutePattern{}, &tool.requirements, &why)) {
        return *error = at + ": " + why, false;
      }
      m->tools.push_back(std::move(tool));
    }
  }
  return true;
}

bool is_role_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// A role in a requirement: "host:name", or "role" or "role/scope", where any part
// may hold {param} (a path parameter of this route), {!tenant}, or {!user_id}.
bool check_role_template(std::string_view role, const router::RoutePattern& pattern, std::string* why) {
  std::string_view body = role;
  bool host = body.starts_with("host:");
  if (host) body.remove_prefix(5);
  int slashes = 0;
  size_t run = 0;  // characters in the current part
  for (size_t i = 0; i < body.size(); ++i) {
    char c = body[i];
    if (c == '{') {
      size_t close = body.find('}', i);
      if (close == std::string_view::npos) return *why = "an unclosed { in \"" + std::string(role) + "\"", false;
      std::string name(body.substr(i + 1, close - i - 1));
      if (name.starts_with("!")) {
        if (name != "!tenant" && name != "!user_id") {
          return *why = "{" + name + "} is not a reserved identifier v1 knows ({!tenant}, {!user_id})", false;
        }
      } else {
        bool found = false;
        for (const auto& seg : pattern.segments) found = found || (!seg.literal && seg.text == name);
        if (!found) return *why = "{" + name + "} is not a parameter of " + pattern.source, false;
      }
      run += 1;
      i = close;
    } else if (c == '/') {
      if (host || ++slashes > 1 || run == 0) return *why = "\"" + std::string(role) + "\" is not role or role/scope", false;
      run = 0;
    } else if (is_role_char(c)) {
      run += 1;
    } else {
      return *why = "\"" + std::string(role) + "\" has a character roles can't hold", false;
    }
  }
  if (run == 0) return *why = "\"" + std::string(role) + "\" is not role or role/scope", false;
  return true;
}

}  // namespace

bool parse_requirements(const bj::value& v, const router::RoutePattern& pattern,
                        std::vector<std::vector<std::string>>* out, std::string* why) {
  if (!v.is_array()) return *why = "requirements must be a list", false;
  const auto& list = v.get_array();
  bool flat = list.empty() || list[0].is_string();
  auto read_clause = [&](const bj::array& items) {
    std::vector<std::string> clause;
    for (const auto& r : items) {
      if (!r.is_string()) return *why = "requirements must hold role strings, or lists of them", false;
      std::string role(r.get_string());
      if (!check_role_template(role, pattern, why)) return false;
      clause.push_back(role);
    }
    out->push_back(std::move(clause));
    return true;
  };
  if (flat) return read_clause(list);
  for (const auto& c : list) {
    if (!c.is_array()) return *why = "requirements mixes roles and lists of roles", false;
    if (!read_clause(c.get_array())) return false;
  }
  return true;
}

namespace {

const std::set<std::string> kVerbs = {"get", "post", "put", "patch", "delete"};

const std::set<std::string> kPatterns = {"by_id", "by_index", "query", "sql", "by_timer"};
const std::set<std::string> kLaterStrategy = {"table", "id-is", "index", "index-is", "limit", "sql", "params",
                                              "result", "read", "writes", "timezone-field", "active-field", "publish"};

bool is_sql_name(std::string_view n) {
  return base::is_valid_name(n) && !n.starts_with("sqlite_");
}

// A list of column names, or true for all of them.
bool parse_columns(const bj::value& v, bool* all, std::set<std::string>* cols, const std::string& where,
                   std::string* error) {
  if (v.is_bool()) {
    *all = v.get_bool();
    return true;
  }
  if (!v.is_array()) return *error = where + " must be true, false, or a list of columns", false;
  for (const auto& c : v.get_array()) {
    if (!c.is_string() || !is_sql_name(c.get_string())) return *error = where + " must hold column names", false;
    cols->insert(std::string(c.get_string()));
  }
  return true;
}

bool parse_data(const bj::object& data, Manifest* m, std::string* error) {
  for (const auto& provider : data) {
    std::string name(provider.key());
    if (name != "sqlite3") {
      m->warnings.push_back("data." + name + " is accepted but not implemented in v1");
      continue;
    }
    if (!provider.value().is_object()) return *error = "data.sqlite3 must be an object", false;
    for (const auto& entry : provider.value().get_object()) {
      std::string where = "data.sqlite3." + std::string(entry.key());
      if (!base::is_valid_name(entry.key())) return *error = where + ": a strategy name must match [a-z][a-z0-9_]*", false;
      if (!entry.value().is_object()) return *error = where + " must be an object", false;
      const auto& def = entry.value().get_object();
      DataStrategy st;
      st.name = std::string(entry.key());
      std::string err;
      auto schema = get_string(def, "schema", &err);
      auto database = get_string(def, "database", &err);
      auto pattern = get_string(def, "pattern", &err);
      if (!err.empty()) return *error = where + ": " + err, false;
      if (!schema || !database || !pattern) return *error = where + ": schema, database, and pattern are required", false;
      if (!kPatterns.count(*pattern)) return *error = where + ": pattern must be by_id, by_index, query, sql, or by_timer", false;
      std::string bare = *database;
      for (size_t at = bare.find("{!tenant}"); at != std::string::npos; at = bare.find("{!tenant}")) bare.replace(at, 9, "x");
      if (!is_sql_name(bare) || bare.find('{') != std::string::npos) {
        return *error = where + ": database must be a name like guestbook, and may hold {!tenant}", false;
      }
      st.schema = *schema;
      st.database = *database;
      st.pattern = *pattern;
      for (const auto& f : def) {
        std::string key(f.key());
        if (key == "schema" || key == "database" || key == "pattern") continue;
        if (st.runnable() && (key == "tables" || key == "row-limit" || key == "step-budget")) continue;
        if (kLaterStrategy.count(key) || (!st.runnable() && (key == "tables" || key == "row-limit" || key == "step-budget"))) {
          if (st.runnable()) return *error = where + "." + key + " doesn't apply to pattern sql", false;
          continue;
        }
        return *error = where + ": unknown field \"" + key + "\"", false;
      }
      if (!st.runnable()) {
        m->warnings.push_back(where + ": pattern " + st.pattern + " is accepted but not implemented in v1");
        m->data.push_back(std::move(st));
        continue;
      }
      const auto* tables = get_object(def, "tables", &err);
      if (!tables || tables->empty()) return *error = where + ": pattern sql needs tables, saying what it may touch", false;
      for (const auto& t : *tables) {
        std::string tw = where + ".tables." + std::string(t.key());
        if (!is_sql_name(t.key())) return *error = tw + ": a table name must match [a-z][a-z0-9_]*", false;
        if (!t.value().is_object()) return *error = tw + " must be an object", false;
        TableGrant g;
        for (const auto& op : t.value().get_object()) {
          std::string k(op.key());
          if (k == "read") {
            if (!parse_columns(op.value(), &g.read_all, &g.read, tw + ".read", error)) return false;
          } else if (k == "update") {
            if (!parse_columns(op.value(), &g.update_all, &g.update, tw + ".update", error)) return false;
          } else if (k == "insert" || k == "delete") {
            if (!op.value().is_bool()) return *error = tw + "." + k + " must be true or false", false;
            (k == "insert" ? g.insert : g.remove) = op.value().get_bool();
          } else {
            return *error = tw + ": unknown operation \"" + k + "\" (read, insert, update, delete)", false;
          }
        }
        st.tables[std::string(t.key())] = std::move(g);
      }
      for (const char* k : {"row-limit", "step-budget"}) {
        auto v = get_int(def, k, &err);
        if (!err.empty()) return *error = where + ": " + err, false;
        if (!v) continue;
        // A step budget is capped too: SQL runs inside a host call, where the request's own
        // deadline can't reach it (the host also stops any statement after two seconds).
        if (*v < 1 || (std::string_view(k) == "row-limit" && *v > 100000) ||
            (std::string_view(k) == "step-budget" && *v > 100'000'000)) {
          return *error = where + "." + k + " is out of range", false;
        }
        (std::string_view(k) == "row-limit" ? st.row_limit : st.step_budget) = *v;
      }
      m->data.push_back(std::move(st));
    }
  }
  return true;
}

bool under_reserved(const router::RoutePattern& p) {
  std::string first = p.segments.empty() || !p.segments[0].literal ? "" : "/" + p.segments[0].text;
  for (const auto& r : reserved_prefixes()) {
    if (first == r) return true;
  }
  return false;
}

}  // namespace

std::optional<Manifest> parse_manifest(std::string_view text, std::string* error) {
  std::string err;
  auto parsed = goober::json::parse(text, &err);
  if (!parsed) return *error = err, std::nullopt;
  if (!parsed->is_object()) return *error = "a manifest must be a JSON object", std::nullopt;
  const auto& root = parsed->get_object();
  Manifest m;

  for (const auto& kv : root) {
    std::string key(kv.key());
    if (key == "box" || key == "mission" || key == "domains" || key == "behavior" || key == "routes" ||
        key == "templates" || key == "data" || key == "types") {
      continue;
    }
    if (key == "config") return *error = "configs (manifests without a box) are not supported in v1", std::nullopt;
    if (kLaterTopLevel.count(key)) {
      m.warnings.push_back("\"" + key + "\" is accepted but not implemented in v1");
      continue;
    }
    return *error = "unknown field \"" + key + "\"", std::nullopt;
  }

  auto box = get_string(root, "box", &err);
  if (!box) return *error = err.empty() ? "\"box\" is required" : err, std::nullopt;
  if (!base::is_valid_name(*box)) return *error = "box name must match [a-z][a-z0-9_]* (at most 63)", std::nullopt;
  m.box = *box;
  m.mission = get_string(root, "mission", &err).value_or("");

  const bj::array* domains = goober::json::get_array(root, "domains", &err);
  if (!domains || domains->empty()) return *error = "\"domains\" is required: a list of domains, or [\"*\"]", std::nullopt;
  for (const auto& d : *domains) {
    if (!d.is_string()) return *error = "\"domains\" must hold strings", std::nullopt;
    m.domains.push_back(base::to_lower(std::string(d.get_string())));
  }

  if (const auto* behavior = get_object(root, "behavior", &err)) {
    for (const auto& kv : *behavior) {
      std::string key(kv.key());
      if (key == "timeout-ms" || key == "fuel" || key == "memory-max-bytes") continue;
      if (kLaterBehavior.count(key)) {
        m.warnings.push_back("behavior." + key + " is accepted but not implemented in v1");
        continue;
      }
      return *error = "unknown field behavior." + key, std::nullopt;
    }
    m.behavior.timeout_ms = get_int(*behavior, "timeout-ms", &err);
    m.behavior.fuel = get_int(*behavior, "fuel", &err);
    m.behavior.memory_max_bytes = get_int(*behavior, "memory-max-bytes", &err);
  }
  if (!err.empty()) return *error = err, std::nullopt;

  if (const auto* templates = get_object(root, "templates", &err)) {
    for (const auto& t : *templates) {
      std::string name(t.key());
      if (!base::is_valid_name(name)) return *error = "template name \"" + name + "\" must match [a-z][a-z0-9_]*", std::nullopt;
      if (!t.value().is_string()) return *error = "templates." + name + " must be a string", std::nullopt;
      std::string why;
      if (!render::check_mustache(t.value().get_string(), &why)) {
        return *error = "templates." + name + ": " + why, std::nullopt;
      }
      m.templates[name] = std::string(t.value().get_string());
    }
  }
  if (!err.empty()) return *error = err, std::nullopt;
  if (auto t = get_string(root, "types", &err)) {
    if (t->empty() || t->size() > 128 || t->find('/') != std::string::npos || !t->ends_with(".json")) {
      return *error = "\"types\" names the types document, a file like orders.openapi.json", std::nullopt;
    }
    m.types = *t;
  }
  if (!err.empty()) return *error = err, std::nullopt;
  if (const auto* data = get_object(root, "data", &err)) {
    if (!parse_data(*data, &m, error)) return std::nullopt;
  }
  if (!err.empty()) return *error = err, std::nullopt;

  std::set<std::string> names;
  router::RouteTable<int> shapes;
  if (const auto* routes = get_object(root, "routes", &err)) {
    for (const auto& proto : *routes) {
      std::string protocol(proto.key());
      if (protocol == "mcp") {
        if (!parse_tools(proto.value(), &names, &m, error)) return std::nullopt;
        continue;
      }
      if (protocol != "http") {
        m.warnings.push_back("routes." + protocol + " is accepted but not implemented in v1");
        continue;
      }
      if (!proto.value().is_object()) return *error = "routes.http must be an object", std::nullopt;
      for (const auto& path : proto.value().get_object()) {
        auto pattern = router::parse_pattern(path.key(), &err);
        if (!pattern) return *error = err, std::nullopt;
        if (under_reserved(*pattern)) {
          return *error = "route " + pattern->source + " is under a host-reserved prefix", std::nullopt;
        }
        if (!path.value().is_object()) return *error = "routes.http." + pattern->source + " must be an object", std::nullopt;
        for (const auto& verb : path.value().get_object()) {
          std::string v(verb.key());
          std::string where = "routes.http." + pattern->source + "." + v;
          if (!kVerbs.count(v)) return *error = where + ": unknown verb (get, post, put, patch, delete)", std::nullopt;
          if (!verb.value().is_object()) return *error = where + " must be an object", std::nullopt;
          const auto& def = verb.value().get_object();
          for (const auto& f : def) {
            std::string key(f.key());
            if (key == "name" || key == "public" || key == "requirements" || key == "template" || key == "agents" ||
                key == "body" || key == "max-body-bytes" || key == "query" || key == "request-headers" ||
                key == "response") {
              continue;
            }
            if (kLaterRoute.count(key)) {
              m.warnings.push_back(where + "." + key + " is accepted but not implemented in v1");
              continue;
            }
            return *error = where + ": unknown field \"" + key + "\"", std::nullopt;
          }
          auto name = get_string(def, "name", &err);
          if (!name) return *error = where + ": \"name\" is required", std::nullopt;
          if (!base::is_valid_name(*name)) {
            return *error = where + ": route name \"" + *name + "\" must match [a-z][a-z0-9_]* (at most 63)", std::nullopt;
          }
          if (!names.insert(*name).second) return *error = "route name \"" + *name + "\" is used twice", std::nullopt;
          if (!shapes.add(*pattern, v, 0)) {
            return *error = where + " conflicts with another route of the same shape", std::nullopt;
          }
          HttpRoute route{v, *name, *pattern, false, {}, {}};
          if (auto t = get_string(def, "template", &err)) {
            if (!base::is_valid_name(*t)) return *error = where + ": template \"" + *t + "\" must match [a-z][a-z0-9_]*", std::nullopt;
            route.template_name = *t;
          }
          if (!err.empty()) return *error = where + ": " + err, std::nullopt;
          route.agents = goober::json::get_bool(def, "agents", &err).value_or(false);
          if (!err.empty()) return *error = where + ": " + err, std::nullopt;
          if (!parse_request_parts(def, v, where, &route, error)) return std::nullopt;
          auto is_public = goober::json::get_bool(def, "public", &err);
          if (!err.empty()) return *error = where + ": " + err, std::nullopt;
          auto req = def.find("requirements");
          if (is_public.value_or(false) && req != def.end()) {
            return *error = where + ": a route can't be both public and have requirements", std::nullopt;
          }
          if (req != def.end()) {
            std::string why;
            if (!parse_requirements(req->value(), *pattern, &route.requirements, &why)) {
              return *error = where + ": " + why, std::nullopt;
            }
          } else if (is_public.value_or(false)) {
            route.is_public = true;
          } else {
            return *error = where + ": needs \"public\": true or \"requirements\" ([] is any signed-in user); "
                                    "a route with neither is denied to everyone",
                   std::nullopt;
          }
          m.routes.push_back(std::move(route));
        }
      }
    }
  }
  if (!err.empty()) return *error = err, std::nullopt;
  return m;
}

namespace {
bool parse_requirements_value(const bj::value& v, const router::RoutePattern& p, std::vector<std::vector<std::string>>* out,
                              std::string* why) {
  return parse_requirements(v, p, out, why);
}
}  // namespace

bool check_types(const Manifest& m, const types::Types* t, std::string* error) {
  auto need = [&](const std::string& ref, const std::string& where) {
    std::string name = types::json_type_name(ref);
    if (name.empty()) return true;
    if (!t) {
      *error = where + " names " + ref + ", but the version has no types document" +
               (m.types.empty() ? " (add \"types\")" : " (" + m.types + " wasn't supplied)");
      return false;
    }
    if (!t->has(name)) {
      *error = where + " names " + ref + ", which isn't in " + m.types;
      return false;
    }
    return true;
  };
  for (const auto& r : m.routes) {
    std::string where = "routes.http." + r.pattern.source + "." + r.verb;
    if (!need(r.body, where + ".body") || !need(r.response, where + ".response")) return false;
  }
  for (const auto& tool : m.tools) {
    std::string where = "routes.mcp." + tool.name + ".tool";
    if (!need(tool.input, where + ".input") || !need(tool.response, where + ".response")) return false;
  }
  if (t && m.types.empty()) return *error = "a types document came with a manifest that names none", false;
  return true;
}

std::string DataStrategy::database_for(std::string_view tenant) const {
  std::string out = database;
  for (size_t at = out.find("{!tenant}"); at != std::string::npos; at = out.find("{!tenant}", at)) {
    out.replace(at, 9, tenant);
    at += tenant.size();
  }
  return out;
}

Access check_access(const HttpRoute& route, const Caller* caller, const std::map<std::string, std::string>& params,
                    std::string_view tenant) {
  if (route.is_public) return Access::kAllowed;
  if (!caller) return Access::kSignIn;
  bool bad_value = false;
  auto substitute = [&](const std::string& role) {
    std::string out;
    for (size_t i = 0; i < role.size(); ++i) {
      if (role[i] != '{') {
        out += role[i];
        continue;
      }
      size_t close = role.find('}', i);
      std::string name = role.substr(i + 1, close - i - 1);
      std::string value;
      if (name == "!tenant") {
        value = tenant;
      } else if (name == "!user_id") {
        value = caller->user_id;
      } else if (auto it = params.find(name); it != params.end()) {
        value = it->second;
      }
      // No substituted value may smuggle in a scope or a namespace.
      if (value.find('/') != std::string::npos || value.find(':') != std::string::npos) bad_value = true;
      out += value;
      i = close;
    }
    return out;
  };
  for (const auto& clause : route.requirements) {
    bool applies = true, holds = true;
    if (caller->host && clause.empty()) applies = false;
    for (const auto& role : clause) {
      // A host identity satisfies only clauses that name host roles, and a tenant
      // identity only clauses that don't.
      if (role.starts_with("host:") != caller->host) applies = false;
      std::string want = substitute(role);
      if (std::find(caller->roles.begin(), caller->roles.end(), want) == caller->roles.end()) holds = false;
    }
    if (bad_value) return Access::kBadValue;
    if (applies && holds) return Access::kAllowed;
  }
  return Access::kForbidden;
}

}  // namespace goober::manifest
