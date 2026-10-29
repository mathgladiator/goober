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
#include "goober/types/types.h"

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
  // What the request may carry (037.Http.md § Everything a request can carry). The host
  // checks all of it before the box wakes up.
  std::string body;  // "", "json:$Type", "text", or "binary"
  std::optional<int64_t> max_body_bytes;
  bool query_declared = false;  // with a query block, the box sees only those parameters, typed
  std::vector<std::pair<std::string, types::SchemaPtr>> query;
  bool headers_declared = false;  // with request-headers, the box sees only those headers
  std::vector<std::string> request_headers;
  std::string response;  // "", "json", "json:$Type", "text", "html", "markdown", or "empty"
  // Origins whose pages may call this route from a browser (037 § CORS), like
  // https://app.example.com, or "*". Never with credentials: only bearer tokens cross.
  std::vector<std::string> cors;
  // An MCP tool (050.AgentsMCP.md) is a route with verb "tool": its input is a named type,
  // and its description is part of the box's shape.
  std::string description;
  std::string input;  // "json:$Type", or "" for a tool that takes nothing
  // A timer (045.Time.md) is a route with verb "timer", called only by the host: it runs
  // every every_ms, or on cron (in the host's zone, or each row's with a source), or once
  // at each row's at_field. With a source (a by_timer strategy), each active row is a fire.
  std::optional<int64_t> every_ms;
  std::string cron;
  std::string source;
  std::string at_field;
  std::string missed;  // "once" (a catch-up fire after downtime) or "skip"
  std::optional<int64_t> timeout_ms;
  // An identity route (design/machine/021.IdentityEvents.md), verb "identity", is called only by
  // the host, after a sign-in commits: "first-verified" or "signed-in".
  std::string identity_event;
  // A mail route (038; design/machine/024.MailIn.md), verb "receive": its address pattern
  // (support@example.com, support@, *@, reply+{ticket : token}@...), the attachments it takes
  // (more is refused), and whether automatic mail (auto-replies, bounces) reaches it.
  // max_body_bytes is its message size limit.
  std::string mail_address;
  int64_t mail_attachments = 0;
  bool mail_automatic = false;
  // An internal route (039) is reachable only from the boxes it names.
  std::vector<std::string> callers;
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
  // A fixed tenant whose database this is, shared by every domain and box that names it;
  // empty for the request's own tenant ({!tenant}, the default).
  std::string tenant;
  std::string pattern;   // by_id, by_index, query, sql, or by_timer
  std::map<std::string, TableGrant> tables;
  int64_t row_limit = 1000;
  int64_t step_budget = 1'000'000;
  // by_timer (045): the table a timer reads, and the columns it reads as they are.
  std::string table;
  std::string timezone_field;
  std::string active_field;
  bool runnable() const { return pattern == "sql"; }
  // Whether any grant lets the box write.
  bool may_write() const {
    for (const auto& [_, g] : tables) {
      if (g.insert || g.update_all || !g.update.empty() || g.remove) return true;
    }
    return false;
  }
  // Strategies whose schema the host applies to the database: sql and by_timer.
  bool applies_schema() const { return pattern == "sql" || pattern == "by_timer"; }
  // The database's name for a tenant, with {!tenant} filled in.
  std::string database_for(std::string_view tenant) const;
  // The tenant whose database a request for `tenant` reaches: the fixed one, or its own.
  std::string tenant_for(std::string_view tenant) const { return this->tenant.empty() ? std::string(tenant) : this->tenant; }
};

// One method of an external service (030.TheLargerEnvironment.md, 035.TheOutBox.md;
// design/machine/015.Network.md): what the box may call, and how the host sends it.
struct ExternalMethod {
  std::string name;
  std::string verb;  // get, post, put, patch, delete
  std::string path;  // like /v1/messages/{message_id}, appended to the service's base
  std::string effect;  // "none" (sent now), "outbox" (after commit), or "immediate" (now, effectful)
  std::string idempotency_header;  // like Idempotency-Key, where the host puts the element's key
  int64_t attempts = 8;
  int64_t backoff_ms = 1000;
  int64_t within_ms = 600000;
  int64_t budget_calls = 10;  // calls per request
  // Fields the host pins, removed from the box's say: "body.to" -> "{!email}", or a literal.
  std::map<std::string, std::string> require;
};

struct ExternalService {
  std::string name;
  std::string box;  // another box on this host: methods name its internal routes (039)
  // The web through the host's crawler (design/machine/017.Crawl.md): the hosts it may read,
  // like "ranch.example", "*.ranch.example", or "*" for any. Its one method is fetch.
  bool web = false;
  std::vector<std::string> web_hosts;
  // Mail through the host's mailer (design/machine/018.Mail.md): its one method is send.
  bool mail = false;
  std::string mail_from_name;
  std::string mail_from;  // "" means the host mailer's address
  std::string base;         // https://api.example.com[/prefix]
  std::string credentials;  // keys/system/$name or keys/tenant/{!tenant}/$name, or ""
  std::string auth_header = "authorization";
  std::string auth_prefix = "Bearer ";
  std::map<std::string, ExternalMethod> methods;
  bool has_outbox() const;
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
  std::vector<HttpRoute> tools;   // routes.mcp.$name.tool, with verb "tool"
  std::vector<HttpRoute> timers;  // routes.timer.$key, with verb "timer"
  std::vector<HttpRoute> internal;  // routes.internal.$path.$verb, reachable only from callers
  std::vector<HttpRoute> identity;  // routes.identity.$event, with verb "identity"
  std::vector<HttpRoute> mail;      // routes.mail.$address.receive, with verb "receive"
  std::string types;             // the types document's file name (036), supplied with the version
  std::map<std::string, std::string> templates;  // Mustache templates by name, checked at upload
  std::vector<DataStrategy> data;
  std::vector<ExternalService> external;
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

// Checks that every json:$Type the manifest names is in its types document, which is
// nullptr when the version came without one.
bool check_types(const Manifest& m, const types::Types* t, std::string* error);

// Parses and checks a manifest. On failure returns nullopt with `error` set.
std::optional<Manifest> parse_manifest(std::string_view text, std::string* error);

}  // namespace goober::manifest
