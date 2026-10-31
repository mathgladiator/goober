#include "goober/manifest/manifest.h"
#include "goober/mail_inbound/address.h"
#include "goober/timers/cron.h"

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

// A key name a manifest may name for a service: keys/system/$name or keys/tenant/{!tenant}/$name,
// and never the host's own (AWS, for SES and backups; the backup and vault keys).
bool service_key_name(std::string_view c) {
  std::string_view rest = c.starts_with("keys/system/")             ? c.substr(12)
                          : c.starts_with("keys/tenant/{!tenant}/") ? c.substr(22)
                                                                     : std::string_view();
  if (!base::is_valid_name(rest)) return false;
  return !(c.starts_with("keys/system/") && (rest.starts_with("aws_") || rest == "backup_key" || rest == "vault_key"));
}

// Fields from 150.DeepManifest.md that v1 accepts with a warning.
const std::set<std::string> kLaterTopLevel = {"pins",    "policy",   "deployment",
                                              "keys", "static",   "headers",
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
  if (auto* c = def.if_contains("cors")) {
    if (!c->is_array() || c->as_array().empty()) return *error = where + ".cors is a list of origins", false;
    for (const auto& o : c->as_array()) {
      std::string origin = o.is_string() ? base::to_lower(std::string(o.as_string())) : "";
      bool ok = origin == "*";
      if (!ok && (origin.starts_with("https://") || origin.starts_with("http://"))) {
        auto rest = origin.substr(origin.find("://") + 3);
        ok = !rest.empty() && rest.find_first_of("/?#@ ") == std::string::npos;
      }
      if (!ok) return *error = where + ".cors holds origins like https://app.example.com, or \"*\"", false;
      route->cors.push_back(origin);
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
      } else if (!def.contains("input")) {
        // Listed with a schema that takes nothing: a tool whose description asks for arguments
        // can't be given them.
        m->warnings.push_back(at + " has no input, so it takes no arguments; give it \"input\": \"json:$Type\" if "
                              "it needs any");
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

// routes.job.$key (design/machine/038.Ids.md § 7): one-time work, like moving data between
// databases, that only an operator starts, in batches. No address: only the host calls it.
bool parse_jobs(const bj::value& jobs, std::set<std::string>* names, Manifest* m, std::string* error) {
  if (!jobs.is_object()) return *error = "routes.job must be an object", false;
  std::string err;
  for (const auto& kv : jobs.as_object()) {
    std::string where = "routes.job." + std::string(kv.key());
    if (!base::is_valid_name(kv.key())) return *error = where + ": a job's key matches [a-z][a-z0-9_]*", false;
    if (!kv.value().is_object()) return *error = where + " must be an object", false;
    const auto& def = kv.value().as_object();
    for (const auto& f : def) {
      std::string k(f.key());
      if (k == "name" || k == "timeout-ms") continue;
      if (k == "requirements" || k == "public") return *error = where + ": jobs are started by an operator and called only by the host, so they carry no " + k, false;
      return *error = where + ": unknown field \"" + k + "\"", false;
    }
    HttpRoute r;
    r.verb = "job";
    auto name = get_string(def, "name", &err);
    if (!name || !base::is_valid_name(*name)) return *error = where + ": name is required, matching [a-z][a-z0-9_]*", false;
    if (!names->insert(*name).second) return *error = "route name \"" + *name + "\" is used twice", false;
    r.name = *name;
    r.timeout_ms = get_int(def, "timeout-ms", &err);
    if (!err.empty()) return *error = where + ": " + err, false;
    if (r.timeout_ms && (*r.timeout_ms < 1 || *r.timeout_ms > 30000)) return *error = where + ".timeout-ms must be 1 to 30000", false;
    m->jobs.push_back(std::move(r));
  }
  return true;
}

// routes.timer.$key (045.Time.md). Timers have no verb, no requirements, and no "public":
// only the host calls them.
bool parse_timers(const bj::value& timers, std::set<std::string>* names, Manifest* m, std::string* error) {
  if (!timers.is_object()) return *error = "routes.timer must be an object", false;
  std::string err;
  for (const auto& kv : timers.as_object()) {
    std::string where = "routes.timer." + std::string(kv.key());
    if (!kv.value().is_object()) return *error = where + " must be an object", false;
    const auto& def = kv.value().as_object();
    for (const auto& f : def) {
      std::string k(f.key());
      if (k == "name" || k == "every-ms" || k == "cron" || k == "source" || k == "at-field" || k == "timeout-ms" ||
          k == "missed") {
        continue;
      }
      if (k == "requirements" || k == "public") return *error = where + ": timers are called only by the host, so they carry no " + k, false;
      if (k == "target" || k == "tenant-field") {
        m->warnings.push_back(where + "." + k + " is accepted but not implemented in v1");
        continue;
      }
      return *error = where + ": unknown field \"" + k + "\"", false;
    }
    HttpRoute t;
    t.verb = "timer";
    auto name = get_string(def, "name", &err);
    if (!name || !base::is_valid_name(*name)) return *error = where + ": name is required, matching [a-z][a-z0-9_]*", false;
    if (!names->insert(*name).second) return *error = "route name \"" + *name + "\" is used twice", false;
    t.name = *name;
    t.every_ms = get_int(def, "every-ms", &err);
    t.cron = get_string(def, "cron", &err).value_or("");
    t.source = get_string(def, "source", &err).value_or("");
    t.at_field = get_string(def, "at-field", &err).value_or("");
    t.timeout_ms = get_int(def, "timeout-ms", &err);
    t.missed = get_string(def, "missed", &err).value_or("");
    if (!err.empty()) return *error = where + ": " + err, false;
    int kinds = int(t.every_ms.has_value()) + int(!t.cron.empty()) + int(!t.at_field.empty());
    if (kinds != 1) return *error = where + ": give exactly one of every-ms, cron, or at-field", false;
    if (t.every_ms && (*t.every_ms < 1000 || *t.every_ms > 31LL * 24 * 3600 * 1000)) {
      return *error = where + ".every-ms must be 1000 (a second) to 31 days", false;
    }
    if (!t.cron.empty()) {
      std::string why;
      if (!timers::Cron::parse(t.cron, &why)) return *error = where + ".cron: " + why, false;
    }
    if (!t.at_field.empty() && (t.source.empty() || !base::is_valid_name(t.at_field))) {
      return *error = where + ": at-field names a column of the source, so it needs a source", false;
    }
    if (t.every_ms && !t.source.empty()) return *error = where + ": a source is scheduled by cron or at-field, not every-ms", false;
    if (t.missed.empty()) t.missed = t.every_ms ? "skip" : "once";
    if (t.missed != "once" && t.missed != "skip") return *error = where + ".missed must be once or skip", false;
    if (t.timeout_ms && (*t.timeout_ms < 1 || *t.timeout_ms > 10000)) return *error = where + ".timeout-ms must be 1 to 10000", false;
    m->timers.push_back(std::move(t));
  }
  return true;
}

// routes.result.$name (035 § results): the host calls the route with an outbox element's
// outcome once it is delivered or given up on. Like timers, only the host calls it.
bool parse_results(const bj::value& routes, std::set<std::string>* names, Manifest* m, std::string* error) {
  if (!routes.is_object()) return *error = "routes.result must be an object", false;
  std::string err;
  for (const auto& kv : routes.as_object()) {
    std::string name(kv.key());
    std::string where = "routes.result." + name;
    if (!base::is_valid_name(name)) return *error = where + ": a result route's key is its name, [a-z][a-z0-9_]*", false;
    if (!kv.value().is_object()) return *error = where + " must be an object", false;
    for (const auto& f : kv.value().as_object()) {
      if (f.key() == "requirements" || f.key() == "public") {
        return *error = where + ": result routes are called only by the host, so they carry no " + std::string(f.key()), false;
      }
      if (f.key() != "timeout-ms") return *error = where + ": unknown field \"" + std::string(f.key()) + "\"", false;
    }
    if (!names->insert(name).second) return *error = "route name \"" + name + "\" is used twice", false;
    HttpRoute r;
    r.verb = "result";
    r.name = name;
    r.timeout_ms = get_int(kv.value().as_object(), "timeout-ms", &err);
    if (!err.empty()) return *error = where + ": " + err, false;
    if (r.timeout_ms && (*r.timeout_ms < 1 || *r.timeout_ms > 10000)) return *error = where + ".timeout-ms must be 1 to 10000", false;
    m->results.push_back(std::move(r));
  }
  return true;
}

// routes.identity.$event (design/machine/021.IdentityEvents.md): the host calls the route
// after a sign-in in the realm of a domain the box serves. Like timers, only the host calls it.
bool parse_identity(const bj::value& routes, std::set<std::string>* names, Manifest* m, std::string* error) {
  if (!routes.is_object()) return *error = "routes.identity must be an object", false;
  std::string err;
  for (const auto& kv : routes.as_object()) {
    std::string event(kv.key());
    std::string where = "routes.identity." + event;
    if (event != "first-verified" && event != "signed-in" && event != "invite-accepted" && event != "invite-declined") {
      return *error = where + ": the identity events are first-verified, signed-in, invite-accepted, and invite-declined", false;
    }
    if (!kv.value().is_object()) return *error = where + " must be an object", false;
    const auto& def = kv.value().as_object();
    for (const auto& f : def) {
      std::string k(f.key());
      if (k == "name" || k == "timeout-ms") continue;
      if (k == "requirements" || k == "public") return *error = where + ": identity routes are called only by the host, so they carry no " + k, false;
      return *error = where + ": unknown field \"" + k + "\"", false;
    }
    HttpRoute r;
    r.verb = "identity";
    r.identity_event = event;
    auto name = get_string(def, "name", &err);
    if (!name || !base::is_valid_name(*name)) return *error = where + ": name is required, matching [a-z][a-z0-9_]*", false;
    if (!names->insert(*name).second) return *error = "route name \"" + *name + "\" is used twice", false;
    r.name = *name;
    r.timeout_ms = get_int(def, "timeout-ms", &err);
    if (!err.empty()) return *error = where + ": " + err, false;
    if (r.timeout_ms && (*r.timeout_ms < 1 || *r.timeout_ms > 10000)) return *error = where + ".timeout-ms must be 1 to 10000", false;
    m->identity.push_back(std::move(r));
  }
  return true;
}

// routes.mail.$address.receive (038.Mail.md; design/machine/024.MailIn.md). People who send
// mail aren't signed in, so a mail route is public; what protects it is the sender's verified
// claims and any token in the address.
constexpr int64_t kMailMaxBytes = 25LL << 20;
bool parse_mail(const bj::value& routes, std::set<std::string>* names, Manifest* m, std::string* error) {
  if (!routes.is_object()) return *error = "routes.mail must be an object", false;
  std::string err;
  std::set<std::string> shapes;
  for (const auto& kv : routes.as_object()) {
    std::string address(kv.key());
    std::string where = "routes.mail." + address;
    auto pattern = mail::inbound::parse_address_pattern(address, &err);
    if (!pattern) return *error = where + ": " + err, false;
    if (!shapes.insert(pattern->shape()).second) return *error = where + " claims the same addresses as another route", false;
    if (!pattern->domain.empty() && !m->applies_to(pattern->domain)) {
      return *error = where + ": " + pattern->domain + " isn't one of the box's domains", false;
    }
    if (!kv.value().is_object()) return *error = where + " must be an object", false;
    for (const auto& verb : kv.value().as_object()) {
      if (verb.key() != "receive") return *error = where + ": a mail route's one verb is receive", false;
    }
    const auto* receive = kv.value().as_object().if_contains("receive");
    if (!receive || !receive->is_object()) return *error = where + ".receive must be an object", false;
    const auto& def = receive->as_object();
    where += ".receive";
    for (const auto& f : def) {
      std::string k(f.key());
      if (k == "name" || k == "public" || k == "max-bytes" || k == "attachments" || k == "automatic" || k == "timeout-ms") continue;
      if (k == "requirements") return *error = where + ": senders aren't signed in, so a mail route is public", false;
      return *error = where + ": unknown field \"" + k + "\"", false;
    }
    HttpRoute r;
    r.verb = "receive";
    r.mail_address = address;
    auto name = get_string(def, "name", &err);
    if (!name || !base::is_valid_name(*name)) return *error = where + ": name is required, matching [a-z][a-z0-9_]*", false;
    if (!names->insert(*name).second) return *error = "route name \"" + *name + "\" is used twice", false;
    r.name = *name;
    if (!goober::json::get_bool(def, "public", &err).value_or(false)) {
      return *error = where + ": needs \"public\": true (anyone can send mail; the sender's claims and tokens are what protect it)", false;
    }
    r.is_public = true;
    r.max_body_bytes = get_int(def, "max-bytes", &err).value_or(10LL << 20);
    r.mail_attachments = get_int(def, "attachments", &err).value_or(0);
    r.mail_automatic = goober::json::get_bool(def, "automatic", &err).value_or(false);
    r.timeout_ms = get_int(def, "timeout-ms", &err);
    if (!err.empty()) return *error = where + ": " + err, false;
    if (*r.max_body_bytes < 1024 || *r.max_body_bytes > kMailMaxBytes) return *error = where + ".max-bytes must be 1024 to 26214400 (25 MiB)", false;
    if (r.mail_attachments < 0 || r.mail_attachments > 100) return *error = where + ".attachments must be 0 to 100", false;
    if (r.timeout_ms && (*r.timeout_ms < 1 || *r.timeout_ms > 10000)) return *error = where + ".timeout-ms must be 1 to 10000", false;
    m->mail.push_back(std::move(r));
  }
  return true;
}

bool valid_identifier(std::string_view s) {
  if (s.empty() || s.size() > 64 || (s[0] >= '0' && s[0] <= '9')) return false;
  for (char c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  }
  return true;
}

// external.$service (030; 035): an https base, credentials by name, and methods.
bool parse_external(const bj::object& ext, Manifest* m, std::string* error) {
  std::string err;
  for (const auto& kv : ext) {
    std::string where = "external." + std::string(kv.key());
    if (!base::is_valid_name(kv.key())) return *error = where + ": a service label must match [a-z][a-z0-9_]*", false;
    if (!kv.value().is_object()) return *error = where + " must be an object", false;
    const auto& def = kv.value().as_object();
    ExternalService svc;
    svc.name = std::string(kv.key());
    for (const auto& f : def) {
      std::string k(f.key());
      if (k == "base" || k == "credentials" || k == "methods" || k == "auth" || k == "spec" || k == "connect" ||
          k == "headers" || k == "limits") {
        continue;
      }
      if (k == "openapi" || k == "operation") {
        m->warnings.push_back(where + "." + k + " is accepted but not read in v1; methods declare verb and path");
        continue;
      }
      if (k == "box" || k == "web" || k == "mail" || k == "push" || k == "invites") continue;
      if (k == "capability" || k == "provider") return *error = where + "." + k + " isn't built yet", false;
      return *error = where + ": unknown field \"" + k + "\"", false;
    }
    if (const auto* mail = def.if_contains("mail")) {
      // Mail through the host's mailer, after commit, through the outbox (design/machine/018.Mail.md).
      if (def.contains("base") || def.contains("credentials") || def.contains("auth") || def.contains("box") ||
          def.contains("web")) {
        return *error = where + ": a mail service has no base, credentials, box, or web; the host sends", false;
      }
      if (!mail->is_object()) return *error = where + ".mail is {\"from-name\": \"Beef Share\"}", false;
      for (const auto& f : mail->as_object()) {
        if (f.key() != "from-name" && f.key() != "from" && f.key() != "lists") {
          return *error = where + ".mail: unknown field \"" + std::string(f.key()) + "\"", false;
        }
      }
      // Lists people opt in to (029 § 4): {"digest": {"title": "The daily digest", "about": "..."}}.
      if (const auto* lists = mail->as_object().if_contains("lists")) {
        if (!lists->is_object() || lists->as_object().size() > 20) {
          return *error = where + ".mail.lists is up to 20 lists: {\"digest\": {\"title\": \"...\", \"about\": \"...\"}}", false;
        }
        for (const auto& l : lists->as_object()) {
          std::string lw = where + ".mail.lists." + std::string(l.key());
          if (!base::is_valid_name(l.key())) return *error = lw + ": a list name matches [a-z][a-z0-9_]*", false;
          const auto* lo = l.value().if_object();
          if (!lo) return *error = lw + " is {\"title\", \"about\"}", false;
          for (const auto& f : *lo) {
            if (f.key() != "title" && f.key() != "about") return *error = lw + ": unknown field \"" + std::string(f.key()) + "\"", false;
          }
          ExternalService::MailList ml{get_string(*lo, "title", &err).value_or(""), get_string(*lo, "about", &err).value_or("")};
          if (ml.title.empty() || ml.title.size() > 80 || ml.about.size() > 300 ||
              ml.title.find_first_of("\r\n") != std::string::npos) {
            return *error = lw + ": title is one line of 1 to 80 characters, and about at most 300", false;
          }
          svc.mail_lists[std::string(l.key())] = std::move(ml);
        }
      }
      svc.mail = true;
      svc.mail_from_name = get_string(mail->as_object(), "from-name", &err).value_or("");
      // The address mail comes from, on one of the tenant's own domains (checked when sending,
      // against the host config); replies to it reach the box's mail routes.
      svc.mail_from = base::to_lower(get_string(mail->as_object(), "from", &err).value_or(""));
      if (!svc.mail_from.empty()) {
        auto at = svc.mail_from.find('@');
        bool ok = at != std::string::npos && at > 0 && at <= 64 && svc.mail_from.size() <= 254 &&
                  svc.mail_from.find('@', at + 1) == std::string::npos &&
                  svc.mail_from.find('.', at) != std::string::npos;
        for (char c : svc.mail_from) {
          if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '@' || c == '.' || c == '-' || c == '_' || c == '+')) ok = false;
        }
        if (!ok) return *error = where + ".mail.from is one address, like hello@example.com", false;
      }
      if (svc.mail_from_name.size() > 64 || svc.mail_from_name.find_first_of("\"\\\r\n<>") != std::string::npos) {
        return *error = where + ".mail.from-name is at most 64 characters, without quotes, backslashes, or <>", false;
      }
      ExternalMethod send;
      send.name = "send";
      send.verb = "mail";
      send.effect = "outbox";
      send.budget_calls = 1;
      send.attempts = 6;
      send.backoff_ms = 30000;
      send.within_ms = 24LL * 3600 * 1000;
      if (const auto* methods = get_object(def, "methods", &err)) {
        for (const auto& mk : *methods) {
          std::string mw = where + ".methods." + std::string(mk.key());
          if (mk.key() != "send") {
            return *error = mw + ": a mail service's methods are send, and attachment, subscribe, and preferences (always there)", false;
          }
          if (!mk.value().is_object()) return *error = mw + " must be an object", false;
          for (const auto& f : mk.value().as_object()) {
            if (f.key() != "budget") return *error = mw + ": unknown field \"" + std::string(f.key()) + "\"", false;
            if (const auto* b = get_object(mk.value().as_object(), "budget", &err)) {
              send.budget_calls = get_int(*b, "calls", &err).value_or(send.budget_calls);
            }
          }
        }
      }
      if (!err.empty()) return *error = where + ": " + err, false;
      if (send.budget_calls < 1 || send.budget_calls > 10) return *error = where + ".methods.send.budget.calls is 1 to 10", false;
      svc.methods["send"] = std::move(send);
      // Reading a received message's attachment by handle (design/machine/024.MailIn.md § 4).
      ExternalMethod attachment;
      attachment.name = "attachment";
      attachment.verb = "mail";
      attachment.effect = "none";
      attachment.budget_calls = 20;
      svc.methods["attachment"] = std::move(attachment);
      // Asking someone to confirm a list (a host mail, after commit), and reading what an address
      // may get (029 § 4).
      ExternalMethod subscribe;
      subscribe.name = "subscribe";
      subscribe.verb = "mail";
      subscribe.effect = "outbox";
      subscribe.budget_calls = 5;
      subscribe.attempts = 6;
      subscribe.backoff_ms = 30000;
      subscribe.within_ms = 24LL * 3600 * 1000;
      svc.methods["subscribe"] = std::move(subscribe);
      ExternalMethod preferences;
      preferences.name = "preferences";
      preferences.verb = "mail";
      preferences.effect = "none";
      preferences.budget_calls = 50;
      svc.methods["preferences"] = std::move(preferences);
      m->external.push_back(std::move(svc));
      continue;
    }
    if (const auto* inv = def.if_contains("invites")) {
      // Invites through the host (design/machine/029.Invites.md § 2.6): the host keeps the record,
      // the template, and the limits; the box names the person and what accepting grants.
      if (def.size() != 1) return *error = where + ": an invites service has only \"invites\"; the host sends", false;
      if (!inv->is_object()) return *error = where + ".invites is {\"roles\": [\"member\"], \"lists\": [\"digest\"]}", false;
      for (const auto& f : inv->as_object()) {
        if (f.key() != "roles" && f.key() != "lists") {
          return *error = where + ".invites: unknown field \"" + std::string(f.key()) + "\"", false;
        }
      }
      svc.invites = true;
      auto names = [&](const char* key, std::vector<std::string>* out, bool role) {
        const auto* v = inv->as_object().if_contains(key);
        if (!v) return true;
        if (!v->is_array() || v->as_array().size() > 20) return false;
        for (const auto& e : v->as_array()) {
          std::string s = e.is_string() ? std::string(e.as_string()) : "";
          bool ok = role ? !s.empty() && s.size() <= 64 && s.find(':') == std::string::npos : base::is_valid_name(s);
          if (role) {
            for (char ch : s) ok = ok && (std::isalnum((unsigned char)ch) || ch == '_' || ch == '/');
          }
          if (!ok) return false;
          out->push_back(s);
        }
        return true;
      };
      if (!names("roles", &svc.invite_roles, true)) {
        return *error = where + ".invites.roles holds up to 20 tenant roles, like member or editor/blog", false;
      }
      if (!names("lists", &svc.invite_lists, false)) {
        return *error = where + ".invites.lists holds up to 20 list names of this box's mail service", false;
      }
      for (const char* method : {"send", "revoke", "status"}) {
        ExternalMethod em;
        em.name = method;
        em.verb = "invites";
        em.effect = std::string(method) == "send" ? "outbox" : "none";
        em.budget_calls = std::string(method) == "status" ? 50 : 10;
        em.attempts = 6;
        em.backoff_ms = 30000;
        em.within_ms = 24LL * 3600 * 1000;
        svc.methods[method] = std::move(em);
      }
      m->external.push_back(std::move(svc));
      continue;
    }
    if (const auto* push = def.if_contains("push")) {
      // Web Push to the tenant's subscribers, after commit, through the outbox (782.PWA.md § 5).
      if (def.contains("base") || def.contains("credentials") || def.contains("auth") || def.contains("box") ||
          def.contains("web") || def.contains("mail")) {
        return *error = where + ": a push service has no base, credentials, box, web, or mail; the host sends", false;
      }
      if (!push->is_object() || !push->as_object().empty()) return *error = where + ".push is {}", false;
      svc.push = true;
      ExternalMethod send;
      send.name = "send";
      send.verb = "push";
      send.effect = "outbox";
      send.budget_calls = 5;
      send.attempts = 5;
      send.backoff_ms = 30000;
      send.within_ms = 6LL * 3600 * 1000;
      if (const auto* methods = get_object(def, "methods", &err)) {
        for (const auto& mk : *methods) {
          std::string mw = where + ".methods." + std::string(mk.key());
          if (mk.key() != "send") return *error = mw + ": a push service's one method is send", false;
          if (!mk.value().is_object()) return *error = mw + " must be an object", false;
          for (const auto& f : mk.value().as_object()) {
            if (f.key() != "budget") return *error = mw + ": unknown field \"" + std::string(f.key()) + "\"", false;
            if (const auto* b = get_object(mk.value().as_object(), "budget", &err)) {
              send.budget_calls = get_int(*b, "calls", &err).value_or(send.budget_calls);
            }
          }
        }
      }
      if (!err.empty()) return *error = where + ": " + err, false;
      if (send.budget_calls < 1 || send.budget_calls > 50) return *error = where + ".methods.send.budget.calls is 1 to 50", false;
      svc.methods["send"] = std::move(send);
      m->external.push_back(std::move(svc));
      continue;
    }
    if (const auto* web = def.if_contains("web")) {
      // Reading the web, politely, through the host: robots.txt, pacing, public addresses only.
      if (def.contains("base") || def.contains("credentials") || def.contains("auth") || def.contains("box")) {
        return *error = where + ": a web service has no base, credentials, or box; the host fetches", false;
      }
      const auto* hosts = web->is_object() ? web->as_object().if_contains("hosts") : nullptr;
      if (!web->is_object() || web->as_object().size() != 1 || !hosts || !hosts->is_array() || hosts->as_array().empty() ||
          hosts->as_array().size() > 1000) {
        return *error = where + ".web is {\"hosts\": [\"ranch.example\", \"*.ranch.example\", or \"*\"]}", false;
      }
      for (const auto& h : hosts->as_array()) {
        std::string host = h.is_string() ? base::to_lower(h.as_string()) : "";
        std::string bare = host.starts_with("*.") ? host.substr(2) : host;
        bool ok = host == "*" || (!bare.empty() && bare.size() <= 253 && bare.find('.') != std::string::npos);
        for (char c : bare) ok &= (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (host == "*") ok = true;
        if (!ok) return *error = where + ".web.hosts: \"" + host + "\" isn't a host name, *.name, or *", false;
        svc.web_hosts.push_back(host);
      }
      svc.web = true;
      ExternalMethod fetch;
      fetch.name = "fetch";
      fetch.verb = "get";
      fetch.effect = "none";
      if (const auto* methods = get_object(def, "methods", &err)) {
        for (const auto& mk : *methods) {
          std::string mw = where + ".methods." + std::string(mk.key());
          if (mk.key() != "fetch") return *error = mw + ": a web service's one method is fetch", false;
          if (!mk.value().is_object()) return *error = mw + " must be an object", false;
          for (const auto& f : mk.value().as_object()) {
            if (f.key() != "budget") return *error = mw + ": unknown field \"" + std::string(f.key()) + "\"", false;
            if (const auto* b = get_object(mk.value().as_object(), "budget", &err)) {
              fetch.budget_calls = get_int(*b, "calls", &err).value_or(fetch.budget_calls);
            }
          }
        }
      }
      if (!err.empty()) return *error = where + ": " + err, false;
      if (fetch.budget_calls < 1 || fetch.budget_calls > 100) return *error = where + ".methods.fetch.budget.calls is 1 to 100", false;
      svc.methods["fetch"] = std::move(fetch);
      m->external.push_back(std::move(svc));
      continue;
    }
    svc.box = get_string(def, "box", &err).value_or("");
    if (!svc.box.empty()) {
      // Another box: its internal routes are the methods, and the host knows the rest (039).
      if (!base::is_valid_name(svc.box)) return *error = where + ".box names a box", false;
      if (def.contains("base") || def.contains("credentials") || def.contains("auth")) {
        return *error = where + ": a box service has no base or credentials; the host routes it", false;
      }
      const auto* methods = get_object(def, "methods", &err);
      if (!methods || methods->empty()) return *error = where + ": needs methods, the box's internal routes it calls", false;
      for (const auto& mk : *methods) {
        std::string mw = where + ".methods." + std::string(mk.key());
        if (!base::is_valid_name(mk.key())) return *error = mw + ": a method is the route name it calls", false;
        if (!mk.value().is_object()) return *error = mw + " must be an object", false;
        ExternalMethod em;
        em.name = std::string(mk.key());
        em.effect = "none";
        for (const auto& f : mk.value().as_object()) {
          std::string k(f.key());
          if (k == "budget") {
            if (const auto* b = get_object(mk.value().as_object(), "budget", &err)) {
              em.budget_calls = get_int(*b, "calls", &err).value_or(em.budget_calls);
            }
          } else if (k == "effect") {
            em.effect = f.value().is_string() ? std::string(f.value().as_string()) : "";
            if (em.effect == "outbox") return *error = mw + ".effect outbox to a box isn't built yet; calls are synchronous", false;
            if (em.effect != "none") return *error = mw + ".effect is none", false;
          } else {
            return *error = mw + ": unknown field \"" + k + "\"", false;
          }
        }
        if (em.budget_calls < 1 || em.budget_calls > 100) return *error = mw + ".budget.calls is 1 to 100", false;
        svc.methods[em.name] = std::move(em);
      }
      m->external.push_back(std::move(svc));
      continue;
    }
    svc.base = get_string(def, "base", &err).value_or("");
    // "*": every call gives its whole URL, on methods marked any-public (034 § 10).
    if (svc.base == "*") {
      svc.any_public = true;
      svc.base = "https://any-public.invalid";
    }
    std::string_view scheme_rest = svc.base.starts_with("https://")                ? std::string_view(svc.base).substr(8)
                                   : svc.base.starts_with("http://127.0.0.1:") ||
                                           svc.base.starts_with("http://localhost:") ? std::string_view(svc.base).substr(7)
                                                                                      : std::string_view();
    if (scheme_rest.empty() || scheme_rest[0] == '/' || svc.base.size() > 256 ||
        svc.base.find_first_of("?#@ \\") != std::string::npos) {
      return *error = where + ".base must be an https URL like https://api.example.com, with no query", false;
    }
    while (svc.base.ends_with("/")) svc.base.pop_back();
    svc.credentials = get_string(def, "credentials", &err).value_or("");
    if (svc.credentials == "sealed") {
      // Each call carries a sealed secret the host made in a connection (034 § 4).
      svc.sealed = true;
      svc.credentials.clear();
      // A sealed secret goes only to the origin it was made for, so never to a URL the box picks.
      if (svc.any_public) return *error = where + ": \"credentials\": \"sealed\" can't go with base \"*\"", false;
    }
    if (!svc.credentials.empty()) {
      std::string_view c = svc.credentials;
      std::string_view rest = c.starts_with("keys/system/") ? c.substr(12)
                              : c.starts_with("keys/tenant/{!tenant}/") ? c.substr(22)
                                                                         : std::string_view();
      if (!base::is_valid_name(rest)) {
        return *error = where + ".credentials names a key: keys/system/$name or keys/tenant/{!tenant}/$name", false;
      }
      // The host's own keys (AWS, for SES and backups; the backup key) are never handed to a box's service.
      if (c.starts_with("keys/system/") && (rest.starts_with("aws_") || rest == "backup_key" || rest == "vault_key")) {
        return *error = where + ".credentials: " + svc.credentials + " is the host's own key", false;
      }
    }
    // A pinned spec (034 § 2): name@sha12.
    if (auto sp = get_string(def, "spec", &err)) {
      auto at = sp->find('@');
      bool ok = at != std::string::npos && at > 0 && sp->size() == at + 13;
      for (size_t i = at + 1; ok && i < sp->size(); ++i) ok = std::isxdigit(static_cast<unsigned char>((*sp)[i])) && !std::isupper(static_cast<unsigned char>((*sp)[i]));
      if (!ok) return *error = where + ".spec pins a stored spec: name@ and the first 12 hex digits of its SHA-256 (spec_put)", false;
      svc.spec = *sp;
    }
    if (const auto* h = get_object(def, "headers", &err)) {
      for (const auto& kv : *h) {
        std::string name = base::to_lower(kv.key());
        bool ok = !name.empty() && kv.value().is_string() && name != "authorization" && name != "host" &&
                  name != "content-length" && name != "content-type";
        for (char ch : name) ok = ok && ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-');
        std::string value = kv.value().is_string() ? std::string(kv.value().as_string()) : "";
        for (char ch : value) ok = ok && static_cast<unsigned char>(ch) >= 0x20;
        if (!ok) return *error = where + ".headers: \"" + name + "\" is a constant header with a text value (not authorization, host, or content-*)", false;
        svc.headers[name] = value;
      }
    }
    if (const auto* lim = get_object(def, "limits", &err)) {
      for (const auto& kv : *lim) {
        if (kv.key() == "retry-after" && kv.value().is_bool()) {
          svc.retry_after = kv.value().as_bool();
        } else if (kv.key() == "remaining" && kv.value().is_string()) {
          svc.limit_remaining = base::to_lower(kv.value().as_string());
        } else if (kv.key() == "reset" && kv.value().is_string()) {
          svc.limit_reset = base::to_lower(kv.value().as_string());
        } else {
          return *error = where + ".limits is {retry-after?: bool, remaining?: header, reset?: header}", false;
        }
      }
    }
    if (const auto* conn = get_object(def, "connect", &err)) {
      if (!svc.sealed) return *error = where + ".connect needs \"credentials\": \"sealed\": a connection makes sealed values", false;
      if (conn->size() != 1 || (!conn->contains("paste") && !conn->contains("oauth2"))) {
        return *error = where + ".connect is {\"paste\": {...}} or {\"oauth2\": {...}}", false;
      }
      auto& cn = svc.connect;
      if (const auto* paste = conn->if_contains("paste")) {
        if (!paste->is_object()) return *error = where + ".connect.paste is {label, help?, check?, result-route}", false;
        cn.kind = "paste";
        for (const auto& f : paste->as_object()) {
          if (f.key() != "label" && f.key() != "help" && f.key() != "check" && f.key() != "result-route") {
            return *error = where + ".connect.paste: unknown field \"" + std::string(f.key()) + "\"", false;
          }
        }
        cn.label = get_string(paste->as_object(), "label", &err).value_or("API key");
        cn.help = get_string(paste->as_object(), "help", &err).value_or("");
        cn.check = get_string(paste->as_object(), "check", &err).value_or("");
        cn.result_route = get_string(paste->as_object(), "result-route", &err).value_or("");
        if (cn.label.size() > 80 || cn.help.size() > 300) return *error = where + ".connect.paste: label up to 80, help up to 300", false;
      } else {
        const auto* o2 = conn->if_contains("oauth2");
        if (!o2->is_object()) return *error = where + ".connect.oauth2 is an object", false;
        cn.kind = "oauth2";
        const auto& o = o2->as_object();
        static const std::set<std::string, std::less<>> kFields = {"authorize", "token", "client-id", "client-secret", "client-auth",
                                                                   "scopes", "scope-separator", "pkce", "extra", "result-route",
                                                                   "early-ms"};
        for (const auto& f : o) {
          if (!kFields.count(f.key())) return *error = where + ".connect.oauth2: unknown field \"" + std::string(f.key()) + "\"", false;
        }
        cn.authorize = get_string(o, "authorize", &err).value_or("");
        cn.token = get_string(o, "token", &err).value_or("");
        if (!cn.authorize.starts_with("https://") || !cn.token.starts_with("https://")) {
          if (!(cn.token.starts_with("http://127.0.0.1:") && cn.authorize.starts_with("http://127.0.0.1:"))) {
            return *error = where + ".connect.oauth2: authorize and token are https URLs", false;
          }
        }
        auto key_name = [&](const char* k, std::string* out) {
          auto v = get_string(o, k, &err);
          if (!v) return true;
          if (!service_key_name(*v)) return false;
          *out = *v;
          return true;
        };
        if (!key_name("client-id", &cn.client_id) || cn.client_id.empty() || !key_name("client-secret", &cn.client_secret)) {
          return *error = where + ".connect.oauth2: client-id (and client-secret) name keys: keys/system/$name or keys/tenant/{!tenant}/$name", false;
        }
        cn.client_auth = get_string(o, "client-auth", &err).value_or("body");
        if (cn.client_auth != "body" && cn.client_auth != "basic") return *error = where + ".connect.oauth2.client-auth is body or basic", false;
        if (auto* sc = o.if_contains("scopes"); sc && sc->is_array()) {
          for (const auto& s : sc->as_array()) {
            if (s.is_string()) cn.scopes.emplace_back(s.as_string());
          }
        }
        cn.scope_separator = get_string(o, "scope-separator", &err).value_or(" ");
        if (auto* pk = o.if_contains("pkce"); pk && pk->is_bool()) cn.pkce = pk->as_bool();
        if (auto* ex = o.if_contains("extra"); ex && ex->is_object()) {
          for (const auto& kv : ex->as_object()) {
            if (kv.value().is_string()) cn.extra[std::string(kv.key())] = std::string(kv.value().as_string());
          }
        }
        cn.result_route = get_string(o, "result-route", &err).value_or("");
        cn.early_ms = get_int(o, "early-ms", &err).value_or(cn.early_ms);
        if (cn.early_ms < 0 || cn.early_ms > 7LL * 24 * 3600 * 1000) return *error = where + ".connect.oauth2.early-ms is up to 7 days", false;
      }
      if (!base::is_valid_name(cn.result_route)) {
        return *error = where + ".connect.*.result-route names the routes.result route that gets the sealed value", false;
      }
    }
    if (const auto* auth = get_object(def, "auth", &err); auth && auth->contains("kind")) {
      // 034 § 3: how the secret is applied.
      svc.auth_kind = get_string(*auth, "kind", &err).value_or("");
      for (const auto& f : *auth) {
        if (f.key() != "kind" && f.key() != "name" && f.key() != "prefix" && f.key() != "user" && f.key() != "consumer-key" &&
            f.key() != "consumer-secret" && f.key() != "token" && f.key() != "token-secret") {
          return *error = where + ".auth: unknown field \"" + std::string(f.key()) + "\"", false;
        }
      }
      if (svc.auth_kind == "header" || svc.auth_kind == "query") {
        auto n = get_string(*auth, "name", &err);
        if (!n || n->empty() || n->size() > 64) return *error = where + ".auth." + svc.auth_kind + " needs a name", false;
        svc.auth_header = svc.auth_kind == "header" ? base::to_lower(*n) : *n;
        svc.auth_prefix = get_string(*auth, "prefix", &err).value_or("");
      } else if (svc.auth_kind == "bearer") {
        svc.auth_header = "authorization";
        svc.auth_prefix = "Bearer ";
      } else if (svc.auth_kind == "basic") {
        svc.auth_header = "authorization";
        svc.basic_user = get_string(*auth, "user", &err).value_or("");
      } else if (svc.auth_kind == "oauth1") {
        svc.oauth1_consumer_key = get_string(*auth, "consumer-key", &err).value_or("");
        svc.oauth1_consumer_secret = get_string(*auth, "consumer-secret", &err).value_or("");
        // An app's own access token (X's portal gives one), when the service isn't sealed per person.
        svc.oauth1_token = get_string(*auth, "token", &err).value_or("");
        svc.oauth1_token_secret = get_string(*auth, "token-secret", &err).value_or("");
        if (svc.oauth1_consumer_key.empty() || svc.oauth1_consumer_secret.empty()) {
          return *error = where + ".auth.oauth1 names consumer-key and consumer-secret keys", false;
        }
        for (const auto* k : {&svc.oauth1_consumer_key, &svc.oauth1_consumer_secret, &svc.oauth1_token, &svc.oauth1_token_secret}) {
          if (!k->empty() && !service_key_name(*k)) {
            return *error = where + ".auth.oauth1: " + *k + " isn't a key a service may use (keys/system/$name or keys/tenant/{!tenant}/$name, not the host's own)", false;
          }
        }
        if (svc.oauth1_token.empty() != svc.oauth1_token_secret.empty()) {
          return *error = where + ".auth.oauth1: token and token-secret go together", false;
        }
      } else {
        return *error = where + ".auth.kind is header, bearer, basic, query, or oauth1", false;
      }
    } else if (const auto* auth2 = get_object(def, "auth", &err)) {
      const auto* auth = auth2;
      if (auto h = get_string(*auth, "header", &err)) svc.auth_header = base::to_lower(*h);
      if (auto p = get_string(*auth, "prefix", &err)) svc.auth_prefix = *p;
      for (char c : svc.auth_header) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return *error = where + ".auth.header is a header name", false;
      }
      for (char c : svc.auth_prefix) {
        if (static_cast<unsigned char>(c) < 0x20) return *error = where + ".auth.prefix holds a control character", false;
      }
    }
    const auto* methods = get_object(def, "methods", &err);
    if (!methods || methods->empty()) return *error = where + ": needs methods, the calls the box may make", false;
    for (const auto& mk : *methods) {
      std::string mw = where + ".methods." + std::string(mk.key());
      if (!valid_identifier(mk.key())) return *error = mw + ": a method name is an identifier", false;
      if (!mk.value().is_object()) return *error = mw + " must be an object", false;
      const auto& md = mk.value().as_object();
      for (const auto& f : md) {
        std::string k(f.key());
        if (k == "verb" || k == "path" || k == "effect" || k == "idempotency" || k == "delivery" || k == "budget" ||
            k == "require" || k == "op" || k == "body" || k == "form-arrays" || k == "any-public" || k == "error-when" ||
            k == "poll" || k == "upload") {
          continue;
        }
        if (k == "result-route") continue;
        return *error = mw + ": unknown field \"" + k + "\"", false;
      }
      ExternalMethod em;
      em.name = std::string(mk.key());
      em.op = get_string(md, "op", &err).value_or("");
      if (!em.op.empty()) {
        if (svc.spec.empty()) return *error = mw + ".op imports an operation of the service's spec, and it has none", false;
        if (md.contains("verb") || md.contains("path")) return *error = mw + ": give op, or verb and path, not both", false;
        if (em.op.size() > 300) return *error = mw + ".op is an operationId or \"VERB /path\"", false;
      }
      em.verb = base::to_lower(get_string(md, "verb", &err).value_or(em.op.empty() ? "" : "post"));
      if (em.verb != "get" && em.verb != "post" && em.verb != "put" && em.verb != "patch" && em.verb != "delete") {
        return *error = mw + ".verb is get, post, put, patch, or delete", false;
      }
      if (!em.op.empty() && em.op.size() > 4 && base::to_lower(em.op.substr(0, 4)) == "get ") em.verb = "get";
      em.effect_given = md.contains("effect");
      em.body = get_string(md, "body", &err).value_or("");
      if (!em.body.empty() && em.body != "json" && em.body != "form" && em.body != "multipart" && em.body != "raw") {
        return *error = mw + ".body is json, form, multipart, or raw", false;
      }
      em.form_arrays = get_string(md, "form-arrays", &err).value_or("brackets");
      if (em.form_arrays != "brackets" && em.form_arrays != "repeat") return *error = mw + ".form-arrays is brackets or repeat", false;
      if (auto* ap = md.if_contains("any-public")) {
        if (!ap->is_bool()) return *error = mw + ".any-public is true or false", false;
        em.any_public = ap->as_bool();
        if (em.any_public && !svc.any_public) return *error = mw + ".any-public needs the service's base to be \"*\"", false;
      }
      auto condition = [&](const bj::value* v, Condition* out, const std::string& what) {
        if (!v) return true;
        if (!v->is_object() || !v->as_object().contains("at") || !v->as_object().at("at").is_string()) {
          *error = mw + "." + what + " is {\"at\": \"body.a.b\", \"present\": true} or {\"at\": ..., \"equals\": value}";
          return false;
        }
        out->at = std::string(v->as_object().at("at").as_string());
        if (!out->at.starts_with("body") && !out->at.starts_with("status")) {
          *error = mw + "." + what + ".at starts with body (or is status)";
          return false;
        }
        if (auto* pr = v->as_object().if_contains("present"); pr && pr->is_bool()) out->present = pr->as_bool();
        if (auto* eq = v->as_object().if_contains("equals")) out->equals = json::dump(*eq);
        if (!out->present && out->equals.empty()) {
          *error = mw + "." + what + " needs present: true or equals";
          return false;
        }
        return true;
      };
      if (!condition(md.if_contains("error-when"), &em.error_when, "error-when")) return false;
      auto strings_map = [&](const bj::object& o, const char* k, std::map<std::string, std::string>* out) {
        if (auto* v = o.if_contains(k); v && v->is_object()) {
          for (const auto& kv : v->as_object()) {
            if (!kv.value().is_string() && !kv.value().is_number()) return false;
            (*out)[std::string(kv.key())] = kv.value().is_string() ? std::string(kv.value().as_string()) : json::dump(kv.value());
          }
        }
        return true;
      };
      if (auto* pv = md.if_contains("poll")) {
        if (!pv->is_object()) return *error = mw + ".poll is an object", false;
        const auto& po = pv->as_object();
        em.polls = true;
        em.poll.op = get_string(po, "op", &err).value_or("");
        if (em.poll.op.empty() || svc.spec.empty()) return *error = mw + ".poll.op names an operation of the service's spec", false;
        if (!strings_map(po, "path", &em.poll.path) || !strings_map(po, "query", &em.poll.query)) {
          return *error = mw + ".poll.path and query map names to text like {body.id}", false;
        }
        if (!condition(po.if_contains("done-when"), &em.poll.done, "poll.done-when")) return false;
        if (!condition(po.if_contains("failed-when"), &em.poll.failed, "poll.failed-when")) return false;
        if (em.poll.done.empty()) return *error = mw + ".poll needs done-when", false;
        em.poll.every_ms = get_int(po, "every-ms", &err).value_or(em.poll.every_ms);
        em.poll.every_from = get_string(po, "every-from", &err).value_or("");
        em.poll.within_ms = get_int(po, "within-ms", &err).value_or(em.poll.within_ms);
        if (em.poll.every_ms < 1000 || em.poll.every_ms > 600000 || em.poll.within_ms < 1000 ||
            em.poll.within_ms > 24LL * 3600 * 1000) {
          return *error = mw + ".poll: every-ms 1000 to 600000, within-ms up to a day", false;
        }
      }
      if (auto* uv = md.if_contains("upload")) {
        if (!uv->is_object()) return *error = mw + ".upload is an object", false;
        const auto& uo = uv->as_object();
        em.upload.attachment_at = get_string(uo, "attachment", &err).value_or("");
        if (!em.upload.attachment_at.starts_with("body.") || svc.spec.empty()) {
          return *error = mw + ".upload.attachment is where the call names the attachment, like body.media, on a service with a spec", false;
        }
        auto step = [&](const char* k, UploadStep* s, bool need) {
          auto* sv = uo.if_contains(k);
          if (!sv) return !need;
          if (!sv->is_object()) return false;
          const auto& so = sv->as_object();
          s->op = get_string(so, "op", &err).value_or("");
          if (s->op.empty()) return false;
          if (!strings_map(so, "path", &s->path) || !strings_map(so, "parts", &s->parts)) return false;
          if (auto* b = so.if_contains("body"); b && b->is_object()) s->body = json::dump(*b);
          s->id_at = get_string(so, "id", &err).value_or("");
          return true;
        };
        if (!step("init", &em.upload.init, true) || !step("append", &em.upload.append, true) || !step("finalize", &em.upload.finalize, true)) {
          return *error = mw + ".upload has init {op, body?, id}, append {op, path?, parts, chunk-bytes?}, and finalize {op, path?}", false;
        }
        if (em.upload.init.id_at.empty()) return *error = mw + ".upload.init.id is where init's answer holds the upload id", false;
        if (auto* ab = uo.at("append").as_object().if_contains("chunk-bytes"); ab && ab->is_int64()) em.upload.chunk_bytes = ab->as_int64();
        if (em.upload.chunk_bytes < 65536 || em.upload.chunk_bytes > (64 << 20)) return *error = mw + ".upload.append.chunk-bytes is 64 KiB to 64 MiB", false;
      }
      em.path = get_string(md, "path", &err).value_or("/");
      if (em.path.empty() || em.path[0] != '/' || em.path.find_first_of("?# \\") != std::string::npos ||
          em.path.find("..") != std::string::npos) {
        return *error = mw + ".path starts with / and holds no query", false;
      }
      for (size_t at = em.path.find('{'); at != std::string::npos; at = em.path.find('{', at + 1)) {
        auto close = em.path.find('}', at);
        if (close == std::string::npos || !valid_identifier(em.path.substr(at + 1, close - at - 1))) {
          return *error = mw + ".path parameters look like {message_id}", false;
        }
      }
      bool safe = em.verb == "get";
      em.effect = get_string(md, "effect", &err).value_or(safe ? "none" : "outbox");
      if ((em.polls || !em.upload.empty()) && (em.effect != "outbox" || em.result_route.empty() && !md.contains("result-route"))) {
        return *error = mw + ": an upload or a poll is an outbox call with a result-route", false;
      }
      if (em.effect != "none" && em.effect != "outbox" && em.effect != "immediate") {
        return *error = mw + ".effect is none, outbox, or immediate", false;
      }
      if (auto idem = get_string(md, "idempotency", &err)) {
        if (!idem->starts_with("header/") || idem->size() < 8) return *error = mw + ".idempotency is header/$Name", false;
        em.idempotency_header = base::to_lower(idem->substr(7));
      }
      if (const auto* rr = md.if_contains("result-route")) {
        if (!rr->is_string() || !base::is_valid_name(rr->as_string())) {
          return *error = mw + ".result-route names a routes.result route", false;
        }
        em.result_route = std::string(rr->as_string());
      }
      if (const auto* d = get_object(md, "delivery", &err)) {
        for (const auto& f : *d) {
          if (f.key() != "attempts" && f.key() != "backoff-ms" && f.key() != "within-ms" && f.key() != "timeout-ms") {
            return *error = mw + ".delivery: unknown field \"" + std::string(f.key()) + "\"", false;
          }
        }
        em.timeout_ms = get_int(*d, "timeout-ms", &err).value_or(em.timeout_ms);
        if (em.timeout_ms < 1000 || em.timeout_ms > 120000) return *error = mw + ".delivery.timeout-ms is 1000 to 120000", false;
        em.attempts = get_int(*d, "attempts", &err).value_or(em.attempts);
        em.backoff_ms = get_int(*d, "backoff-ms", &err).value_or(em.backoff_ms);
        em.within_ms = get_int(*d, "within-ms", &err).value_or(em.within_ms);
        if (em.attempts < 1 || em.attempts > 50 || em.backoff_ms < 100 || em.backoff_ms > 3'600'000 ||
            em.within_ms < 1000 || em.within_ms > 7LL * 24 * 3600 * 1000) {
          return *error = mw + ".delivery: attempts 1-50, backoff-ms 100-3600000, within-ms up to 7 days", false;
        }
      }
      if (const auto* b = get_object(md, "budget", &err)) {
        em.budget_calls = get_int(*b, "calls", &err).value_or(em.budget_calls);
        if (em.budget_calls < 1 || em.budget_calls > 100) return *error = mw + ".budget.calls is 1 to 100", false;
      }
      if (const auto* r = get_object(md, "require", &err)) {
        for (const auto& rk : *r) {
          std::string field(rk.key());
          auto dot = field.find('.');
          std::string part = dot == std::string::npos ? "" : field.substr(0, dot);
          if ((part != "body" && part != "query" && part != "path" && part != "header") || dot + 1 >= field.size()) {
            return *error = mw + ".require keys look like body.to, query.user, path.id, or header.x-user", false;
          }
          if (!rk.value().is_string()) return *error = mw + ".require values are strings", false;
          std::string v(rk.value().as_string());
          if (v.starts_with("{!") && v != "{!email}" && v != "{!user_id}" && v != "{!tenant}" && v != "{!domain}") {
            return *error = mw + ".require may use {!email}, {!user_id}, {!tenant}, or {!domain}", false;
          }
          em.require[field] = v;
        }
      }
      if (!err.empty()) return *error = mw + ": " + err, false;
      svc.methods[em.name] = std::move(em);
    }
    if (!err.empty()) return *error = where + ": " + err, false;
    m->external.push_back(std::move(svc));
  }
  return true;
}

// routes.internal (039): HTTP-shaped routes that only the boxes in callers can reach.
bool parse_internal(const bj::value& routes, std::set<std::string>* names, Manifest* m, std::string* error) {
  if (!routes.is_object()) return *error = "routes.internal must be an object", false;
  std::string err;
  router::RouteTable<int> shapes;
  for (const auto& path : routes.as_object()) {
    auto pattern = router::parse_pattern(path.key(), &err);
    if (!pattern) return *error = "routes.internal: " + err, false;
    if (!path.value().is_object()) return *error = "routes.internal." + pattern->source + " must be an object", false;
    for (const auto& verb : path.value().as_object()) {
      std::string v(verb.key());
      std::string where = "routes.internal." + pattern->source + "." + v;
      if (v != "get" && v != "post" && v != "put" && v != "patch" && v != "delete") {
        return *error = where + ": unknown verb (get, post, put, patch, delete)", false;
      }
      if (!verb.value().is_object()) return *error = where + " must be an object", false;
      const auto& def = verb.value().as_object();
      for (const auto& f : def) {
        std::string k(f.key());
        if (k == "name" || k == "callers" || k == "requirements" || k == "body" || k == "query" || k == "response" ||
            k == "max-body-bytes") {
          continue;
        }
        if (k == "public") return *error = where + ": nothing outside can reach an internal route, so it isn't public", false;
        if (k == "idempotent" || k == "timeout-ms") {
          m->warnings.push_back(where + "." + k + " is accepted but not implemented in v1");
          continue;
        }
        return *error = where + ": unknown field \"" + k + "\"", false;
      }
      HttpRoute r;
      r.verb = v;
      r.pattern = *pattern;
      auto name = get_string(def, "name", &err);
      if (!name || !base::is_valid_name(*name)) return *error = where + ": name is required, matching [a-z][a-z0-9_]*", false;
      if (!names->insert(*name).second) return *error = "route name \"" + *name + "\" is used twice", false;
      r.name = *name;
      if (!shapes.add(*pattern, v, 0)) return *error = where + " conflicts with another internal route", false;
      const auto* callers = goober::json::get_array(def, "callers", &err);
      if (!callers || callers->empty()) {
        return *error = where + ": needs callers, the boxes that may call it; without them nothing can", false;
      }
      for (const auto& c : *callers) {
        if (!c.is_string() || !base::is_valid_name(c.as_string())) return *error = where + ".callers lists box names", false;
        r.callers.emplace_back(c.as_string());
      }
      if (auto* req = def.if_contains("requirements")) {
        std::string why;
        if (!parse_requirements(*req, *pattern, &r.requirements, &why)) return *error = where + ": " + why, false;
      } else {
        r.is_public = true;  // callers alone gate it; there is no outside to be public to
      }
      if (!parse_request_parts(def, v, where, &r, error)) return false;
      m->internal.push_back(std::move(r));
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
                                              "result", "read", "writes", "timezone-field", "active-field"};

// A bus topic template (design/machine/036.Bus.md § 2): 1 to 8 segments joined by "/", each
// [A-Za-z0-9_-]{1,64} or one of {!tenant}, {!user_id}, {!domain}.
bool valid_topic(std::string_view t, std::string* why) {
  size_t n = 0;
  size_t start = 0;
  while (true) {
    size_t slash = t.find('/', start);
    std::string_view seg = t.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
    ++n;
    bool reserved = seg == "{!tenant}" || seg == "{!user_id}" || seg == "{!domain}";
    bool plain = !seg.empty() && seg.size() <= 64;
    for (char c : seg) {
      plain = plain && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
    }
    if (!reserved && !plain) {
      return *why = "topic \"" + std::string(t) + "\": each segment is letters, digits, _ or -, or {!tenant}, {!user_id}, {!domain}", false;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  if (n > 8) return *why = "topic \"" + std::string(t) + "\" has more than 8 segments", false;
  return true;
}

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
      // "tenant": "{!tenant}" is the default; a fixed name shares one database across domains.
      if (auto tenant = get_string(def, "tenant", &err)) {
        if (*tenant != "{!tenant}") {
          if (!base::is_valid_name(*tenant)) {
            return *error = where + ".tenant must be {!tenant} or a tenant name like shared", false;
          }
          if (database->find("{!tenant}") != std::string::npos) {
            return *error = where + ": a database with a fixed tenant can't also hold {!tenant}", false;
          }
          st.tenant = *tenant;
          m->warnings.push_back(where + " reaches tenant " + *tenant +
                                "'s database " + *database + " whatever domain the request came to");
        }
      }
      if (!err.empty()) return *error = where + ": " + err, false;
      for (const auto& f : def) {
        std::string key(f.key());
        if (key == "schema" || key == "database" || key == "pattern" || key == "tenant") continue;
        if (st.pattern == "by_timer" && (key == "table" || key == "timezone-field" || key == "active-field")) continue;
        if (st.runnable() && (key == "tables" || key == "row-limit" || key == "step-budget" || key == "publish")) continue;
        if (kLaterStrategy.count(key) || (!st.runnable() && (key == "tables" || key == "row-limit" || key == "step-budget"))) {
          if (st.runnable()) return *error = where + "." + key + " doesn't apply to pattern sql", false;
          continue;
        }
        return *error = where + ": unknown field \"" + key + "\"", false;
      }
      if (auto pub = get_string(def, "publish", &err)) {
        std::string why;
        if (!valid_topic(*pub, &why)) return *error = where + ".publish: " + why, false;
        st.publish = *pub;
      }
      if (st.pattern == "by_timer") {
        // Read only by the scheduler: the box can't call it from code (045).
        st.table = get_string(def, "table", &err).value_or("");
        st.timezone_field = get_string(def, "timezone-field", &err).value_or("");
        st.active_field = get_string(def, "active-field", &err).value_or("");
        if (!err.empty()) return *error = where + ": " + err, false;
        if (!is_sql_name(st.table)) return *error = where + ": by_timer needs table, a table in its schema", false;
        for (const auto* col : {&st.timezone_field, &st.active_field}) {
          if (!col->empty() && !is_sql_name(*col)) return *error = where + ": a -field names a column", false;
        }
        m->data.push_back(std::move(st));
        continue;
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
        key == "templates" || key == "data" || key == "types" || key == "external" || key == "libraries") {
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
  if (const auto* ext = get_object(root, "external", &err)) {
    if (!parse_external(*ext, &m, error)) return std::nullopt;
    // An invite joins only lists this box's own mail service declares (029 § 2.6).
    std::set<std::string> lists;
    for (const auto& svc : m.external) {
      for (const auto& [name, l] : svc.mail_lists) lists.insert(name);
    }
    for (const auto& svc : m.external) {
      for (const auto& l : svc.invite_lists) {
        if (!lists.count(l)) {
          *error = "external." + svc.name + ".invites.lists names " + l + ", which no mail service of this box declares";
          return std::nullopt;
        }
      }
    }
  }
  if (const auto* libs = get_object(root, "libraries", &err)) {
    for (const auto& kv : *libs) {
      std::string alias(kv.key());
      if (!valid_identifier(alias)) return *error = "libraries." + alias + ": the name is an identifier", std::nullopt;
      std::string ref = kv.value().is_string() ? std::string(kv.value().as_string()) : "";
      auto at = ref.find('@');
      int major = 0;
      bool ok = at != std::string::npos && at > 0 && at + 1 < ref.size();
      for (size_t i = 0; ok && i < at; ++i) {
        char c = ref[i];
        ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
      }
      for (size_t i = at + 1; ok && i < ref.size(); ++i) {
        ok = ref[i] >= '0' && ref[i] <= '9' && major < 1000;
        if (ok) major = major * 10 + (ref[i] - '0');
      }
      if (!ok || major < 1) return *error = "libraries." + alias + " is a library and its major version, like \"time@1\"", std::nullopt;
      m.libraries.push_back({alias, ref.substr(0, at), major});
    }
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
      if (protocol == "timer") {
        if (!parse_timers(proto.value(), &names, &m, error)) return std::nullopt;
        continue;
      }
      if (protocol == "job") {
        if (!parse_jobs(proto.value(), &names, &m, error)) return std::nullopt;
        continue;
      }
      if (protocol == "internal") {
        if (!parse_internal(proto.value(), &names, &m, error)) return std::nullopt;
        continue;
      }
      if (protocol == "identity") {
        if (!parse_identity(proto.value(), &names, &m, error)) return std::nullopt;
        continue;
      }
      if (protocol == "result") {
        if (!parse_results(proto.value(), &names, &m, error)) return std::nullopt;
        continue;
      }
      if (protocol == "mail") {
        if (!parse_mail(proto.value(), &names, &m, error)) return std::nullopt;
        continue;
      }
      if (protocol != "http" && protocol != "sse") {
        m.warnings.push_back("routes." + protocol + " is accepted but not implemented in v1");
        continue;
      }
      bool sse = protocol == "sse";
      if (!proto.value().is_object()) return *error = "routes." + protocol + " must be an object", std::nullopt;
      for (const auto& path : proto.value().get_object()) {
        auto pattern = router::parse_pattern(path.key(), &err);
        if (!pattern) return *error = err, std::nullopt;
        if (under_reserved(*pattern)) {
          return *error = "route " + pattern->source + " is under a host-reserved prefix", std::nullopt;
        }
        if (!path.value().is_object()) return *error = "routes." + protocol + "." + pattern->source + " must be an object", std::nullopt;
        for (const auto& verb : path.value().get_object()) {
          std::string v(verb.key());
          std::string where = "routes." + protocol + "." + pattern->source + "." + v;
          if (sse && v != "subscribe") return *error = where + ": an sse route is {\"subscribe\": {...}}", std::nullopt;
          if (!sse && !kVerbs.count(v)) return *error = where + ": unknown verb (get, post, put, patch, delete)", std::nullopt;
          if (!verb.value().is_object()) return *error = where + " must be an object", std::nullopt;
          const auto& def = verb.value().get_object();
          for (const auto& f : def) {
            std::string key(f.key());
            if (key == "name" || key == "public" || key == "requirements" || key == "template" || key == "agents" ||
                key == "body" || key == "max-body-bytes" || key == "query" || key == "request-headers" ||
                key == "response" || key == "cors" || key == "includes") {
              // A subscription answers its own page's EventSource: no body, no template or
              // partials (the events are the box's bytes), and no cross-origin readers.
              if (sse && (key == "body" || key == "max-body-bytes" || key == "template" || key == "includes" ||
                          key == "cors")) {
                return *error = where + "." + key + " doesn't apply to a subscription", std::nullopt;
              }
              continue;
            }
            if (sse && (key == "wake" || key == "coalesce-ms" || key == "refresh-ms")) continue;
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
          HttpRoute route;
          route.verb = v;
          route.name = *name;
          route.pattern = *pattern;
          if (auto t = get_string(def, "template", &err)) {
            if (!base::is_valid_name(*t)) return *error = where + ": template \"" + *t + "\" must match [a-z][a-z0-9_]*", std::nullopt;
            route.template_name = *t;
          }
          if (!err.empty()) return *error = where + ": " + err, std::nullopt;
          route.agents = goober::json::get_bool(def, "agents", &err).value_or(false);
          if (sse) {
            const auto* w = def.if_contains("wake");
            if (!w || !w->is_array() || w->as_array().empty() || w->as_array().size() > 8) {
              return *error = where + ".wake is a list of 1 to 8 topics, like [\"orders\"]", std::nullopt;
            }
            for (const auto& t : w->as_array()) {
              std::string why;
              if (!t.is_string() || !valid_topic(t.as_string(), &why)) return *error = where + ".wake: " + why, std::nullopt;
              route.wake.emplace_back(t.as_string());
            }
            route.coalesce_ms = get_int(def, "coalesce-ms", &err).value_or(250);
            route.refresh_ms = get_int(def, "refresh-ms", &err).value_or(30000);
            if (route.coalesce_ms < 50 || route.coalesce_ms > 60000) return *error = where + ".coalesce-ms is 50 to 60000", std::nullopt;
            if (route.refresh_ms < 1000 || route.refresh_ms > 3600000) return *error = where + ".refresh-ms is 1000 to 3600000", std::nullopt;
          }
          // The box's own text/html gets the tenant's partials (design/machine/027.Partials.md).
          route.includes = goober::json::get_bool(def, "includes", &err).value_or(false);
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
            for (const auto& t : route.wake) {
              if (t.find("{!user_id}") != std::string::npos) {
                return *error = where + ": a public subscription has no {!user_id} to wake on", std::nullopt;
              }
            }
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
  // A method's result-route is one of this box's result routes, on a method that goes through the outbox.
  for (const auto& svc : m.external) {
    for (const auto& [name, em] : svc.methods) {
      if (em.result_route.empty()) continue;
      std::string mw = "external." + svc.name + ".methods." + name;
      if (em.effect != "outbox") return *error = mw + ".result-route needs effect outbox", std::nullopt;
      bool found = false;
      for (const auto& r : m.results) found |= r.name == em.result_route;
      if (!found) return *error = mw + ".result-route: no routes.result." + em.result_route, std::nullopt;
    }
  }
  for (const auto& t : m.timers) {
    if (t.source.empty()) continue;
    const DataStrategy* st = nullptr;
    for (const auto& d : m.data) {
      if (d.name == t.source) st = &d;
    }
    if (!st || st->pattern != "by_timer") {
      return *error = "routes.timer: " + t.name + "'s source must be a by_timer strategy in data.sqlite3", std::nullopt;
    }
    if (!t.at_field.empty() && st->active_field.empty()) {
      return *error = "routes.timer: " + t.name + " fires once per row, so " + st->name +
                          " needs an active-field the host can turn off after the fire", std::nullopt;
    }
  }
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
  for (const auto& r : m.internal) {
    std::string where = "routes.internal." + r.pattern.source + "." + r.verb;
    if (!need(r.body, where + ".body") || !need(r.response, where + ".response")) return false;
  }
  for (const auto& tool : m.tools) {
    std::string where = "routes.mcp." + tool.name + ".tool";
    if (!need(tool.input, where + ".input") || !need(tool.response, where + ".response")) return false;
  }
  if (t && m.types.empty()) return *error = "a types document came with a manifest that names none", false;
  return true;
}

bool ExternalService::has_outbox() const {
  for (const auto& [_, m] : methods) {
    if (m.effect == "outbox") return true;
  }
  return false;
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
