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
const std::set<std::string> kLaterTopLevel = {"types", "pins",    "policy",   "deployment", "data",
                                              "external", "keys", "static",   "headers",
                                              "rewrites", "counters", "working-set", "content-security"};
const std::set<std::string> kLaterBehavior = {"mode", "owned-by", "lease-ms", "boundary", "lifecycle", "budget",
                                              "instances"};
const std::set<std::string> kLaterRoute = {"response", "body",     "query",     "headers",     "callers",
                                           "signal",   "max-body-bytes", "timeout-ms", "description",
                                           "request-headers", "grantable", "revocable"};

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
        key == "templates") {
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

  std::set<std::string> names;
  router::RouteTable<int> shapes;
  if (const auto* routes = get_object(root, "routes", &err)) {
    for (const auto& proto : *routes) {
      std::string protocol(proto.key());
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
            if (key == "name" || key == "public" || key == "requirements" || key == "template") continue;
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
