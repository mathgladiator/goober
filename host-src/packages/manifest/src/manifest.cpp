#include "goober/manifest/manifest.h"
#include "goober/router/table.h"

#include <algorithm>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/names.h"
#include "goober/json/json.h"

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
      "/goober-operations", "/goober-operations-mcp", "/internal", "/error-message"};
  return kPrefixes;
}

namespace {

// Fields from 150.DeepManifest.md that v1 accepts with a warning.
const std::set<std::string> kLaterTopLevel = {"types", "pins",    "policy",   "deployment", "data",
                                              "external", "keys", "static",   "templates",  "headers",
                                              "rewrites", "counters", "working-set", "content-security"};
const std::set<std::string> kLaterBehavior = {"mode", "owned-by", "lease-ms", "boundary", "lifecycle", "budget",
                                              "instances"};
const std::set<std::string> kLaterRoute = {"response", "requirements", "public", "body",  "query",
                                           "headers",  "callers",      "signal", "max-body-bytes", "timeout-ms",
                                           "description", "request-headers"};
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
    if (key == "box" || key == "mission" || key == "domains" || key == "behavior" || key == "routes") continue;
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
            if (key == "name") continue;
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
          m.routes.push_back(HttpRoute{v, *name, *pattern});
        }
      }
    }
  }
  if (!err.empty()) return *error = err, std::nullopt;
  return m;
}

}  // namespace goober::manifest
