#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"
#include "goober/router/path.h"

namespace goober::manifest {

struct HttpRoute {
  std::string verb;  // get, post, put, patch, delete
  std::string name;  // the route name; the box exports goober_route_$name
  router::RoutePattern pattern;
  // Exactly one of these is set (013.Identity.md): "public": true, or requirements
  // in disjunctive normal form, an OR of AND clauses. An empty clause is any
  // signed-in user of the realm. Roles may hold {param}, {!tenant}, and {!user_id}.
  bool is_public = false;
  std::vector<std::vector<std::string>> requirements;
  // A template that renders the route's JSON for callers that prefer HTML
  // (070.UI.md § One route, many audiences; design/machine/007.Content.md §8).
  std::string template_name;
  // "agents": true lets agent tokens call the route (050.AgentsMCP.md); otherwise only
  // a person, signed in through a browser or a person client, may.
  bool agents = false;
  std::string export_name() const { return "goober_route_" + name; }
};

// Who is calling, as the identity service resolved them.
struct Caller {
  std::string user_id;
  bool host = false;               // a host identity: only clauses naming host roles apply
  std::vector<std::string> roles;  // tenant roles, or host roles for a host identity
};

enum class Access {
  kAllowed,
  kSignIn,     // no caller, and the route isn't public: 401
  kForbidden,  // a caller who satisfies no clause: 403
  kBadValue,   // a substituted value holds "/" or ":": 400
};

// Checks a caller (nullptr when signed out) against a route's requirements.
Access check_access(const HttpRoute& route, const Caller* caller, const std::map<std::string, std::string>& params,
                    std::string_view tenant);

// What a strategy lets a box do with one table (design/machine/008.Databases.md §4).
struct TableGrant {
  bool read_all = false;
  std::set<std::string> read;  // columns, when not read_all
  bool insert = false;
  bool update_all = false;
  std::set<std::string> update;
  bool remove = false;
};

// One strategy at data.sqlite3.$name (020.DataFlow.md). Only pattern "sql" runs in v1.
struct DataStrategy {
  std::string name;
  std::string schema;    // a stored schema's name, like guestbook1
  std::string database;  // a name, possibly holding {!tenant}
  std::string pattern;   // by_id, by_index, query, sql, or by_timer
  std::map<std::string, TableGrant> tables;
  int64_t row_limit = 1000;
  int64_t step_budget = 1'000'000;
  bool runnable() const { return pattern == "sql"; }
  // The database's name for a tenant, with {!tenant} filled in.
  std::string database_for(std::string_view tenant) const;
};

struct Behavior {
  std::optional<int64_t> timeout_ms;
  std::optional<int64_t> fuel;
  std::optional<int64_t> memory_max_bytes;
};

// The v1 subset of a box manifest (design/machine/005.FirstVersion.md §10).
struct Manifest {
  std::string box;
  std::string mission;
  std::vector<std::string> domains;  // "*" or lowercase domain names
  Behavior behavior;
  std::vector<HttpRoute> routes;
  std::map<std::string, std::string> templates;  // Mustache templates by name, checked at upload
  std::vector<DataStrategy> data;
  std::vector<std::string> warnings;  // fields v1 accepts but does not implement

  bool applies_to(std::string_view domain) const;
};

// Parses requirements in disjunctive normal form: a list of roles (one clause), or a
// list of lists of roles. Parameters must belong to `pattern`; content pages pass an
// empty pattern, so only {!tenant} and {!user_id} can be substituted.
bool parse_requirements(const boost::json::value& v, const router::RoutePattern& pattern,
                        std::vector<std::vector<std::string>>* out, std::string* why);

// The URL prefixes no box may declare a route under.
const std::vector<std::string>& reserved_prefixes();

// Parses and checks a manifest. On failure returns nullopt with `error` set.
std::optional<Manifest> parse_manifest(std::string_view text, std::string* error);

}  // namespace goober::manifest
