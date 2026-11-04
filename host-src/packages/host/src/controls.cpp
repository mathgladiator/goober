// Per-domain controls (design/machine/047.Controls.md): what operators set per domain, API, and
// endpoint, kept as storage/controls/$tenant/$domain.json written through the host WAL (so the
// change log has every change, backups carry them, and a restart keeps them), compiled into the
// gate, checked on the request path, and reported per control. Box routes' own `limits` (§ 13)
// are compiled from the active set when it changes and sit between an API's control and an
// operator's endpoint override. The matcher, the merge, and the counting are the controls
// package's; this file stores, wires, and serves them, for the tools and the panel alike.

#include <algorithm>
#include <iostream>

#include "goober/base/bounded.h"
#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/controls/report.h"
#include "goober/host/host.h"
#include "goober/site/includes.h"
#include "http_util.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

namespace {

constexpr const char* kTimeoutHeader = "goober-control-timeout-ms";
constexpr const char* kFuelHeader = "goober-control-fuel";
constexpr int64_t kUserKeyMs = 60 * 1000;
constexpr size_t kUserKeysMax = 10000;

std::string file_of(const std::string& tenant, const std::string& domain) {
  return "controls/" + tenant + "/" + domain + ".json";
}

std::optional<std::string> text_arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

bool one_line(std::string_view s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

// A control's stored entry without its history: what a change goes from and to.
bj::value entry_state(const bj::object* entry) {
  if (!entry) return nullptr;
  bj::object o = *entry;
  o.erase("history");
  return o;
}

std::vector<controls::Change> history_of(const bj::object* entry) {
  std::vector<controls::Change> out;
  if (!entry) return out;
  const auto* h = entry->if_contains("history");
  if (!h || !h->is_array()) return out;
  for (const auto& v : h->as_array()) {
    if (!v.is_object()) continue;
    const auto& o = v.as_object();
    controls::Change c;
    if (const auto* at = o.if_contains("at_ms"); at && at->is_int64()) c.at_ms = at->as_int64();
    if (const auto* by = o.if_contains("by"); by && by->is_string()) c.by = std::string(by->as_string());
    if (const auto* why = o.if_contains("why"); why && why->is_string()) c.why = std::string(why->as_string());
    if (const auto* b = o.if_contains("before")) c.before = *b;
    if (const auto* a = o.if_contains("after")) c.after = *a;
    out.push_back(std::move(c));
  }
  return out;
}

controls::HostLimits limits_of(const config::HostConfig& cfg) {
  return controls::HostLimits{cfg.body_max_bytes, cfg.timeout_ms.max_value, cfg.fuel.max_value};
}

// Adds "control": note to an access line's extra (a JSON object's text, or "").
std::string with_control(const std::string& extra, const std::string& note) {
  if (extra.size() < 2 || extra.front() != '{' || extra.back() != '}') return "{\"control\":" + note + "}";
  if (extra == "{}") return "{\"control\":" + note + "}";
  return extra.substr(0, extra.size() - 1) + ",\"control\":" + note + "}";
}

std::string from_of(const controls::Control& c) {
  if (c.level == controls::Level::kHost) return "host.json controls.defaults";
  if (c.level == controls::Level::kBox) return c.id + " (the box's manifest)";
  return c.id;
}

// Each property along a chain: where its enforced value comes from, and what observes it.
bj::object effective_json(const controls::DomainControls::Chain& chain) {
  bj::object enforced, observed;
  for (auto p : controls::kProperties) {
    auto e = controls::effective(chain, p);
    if (e.enforced) {
      enforced[controls::property_name(p)] =
          bj::object{{"value", controls::property_json(e.enforced->props, p)}, {"from", from_of(*e.enforced)}};
    }
    if (e.observed) {
      observed[controls::property_name(p)] =
          bj::object{{"value", controls::property_json(e.observed->props, p)}, {"from", from_of(*e.observed)}};
    }
  }
  return bj::object{{"enforced", std::move(enforced)}, {"observed", std::move(observed)}};
}

// The configured domain and its tenant, or why not.
std::string resolve_domain(const config::HostConfig& cfg, const bj::object& args, std::string* tenant, std::string* error) {
  std::string d = base::to_lower(text_arg(args, "domain").value_or(""));
  if (d.empty()) return *error = "domain is required", "";
  if (auto r = cfg.redirects.find(d); r != cfg.redirects.end()) {
    return *error = d + " redirects to " + r->second + ": set its controls there", "";
  }
  auto it = cfg.domains.find(d);
  if (it == cfg.domains.end()) return *error = "no domain " + d + " on this host (domain_list names them)", "";
  *tenant = it->second.tenant;
  return d;
}

bj::object load_file(wal::HostWal& wal, const std::string& tenant, const std::string& domain) {
  bj::object file;
  if (auto text = base::read_file(wal.root() / file_of(tenant, domain))) {
    std::string err;
    if (auto v = json::parse(*text, &err); v && v->is_object()) file = v->as_object();
  }
  return file;
}

// Checks the whole file, writes it through the WAL, and puts it in the gate: the next request sees it.
bool keep_file(const config::HostConfig& cfg, wal::HostWal& wal, controls::Gate& gate, const std::string& tenant,
               const std::string& domain, const bj::object& file, std::string* error) {
  auto compiled = controls::compile(file, domain, tenant, cfg.controls_defaults, limits_of(cfg), error);
  if (!compiled) return false;
  bool empty = file.empty() || (file.size() == 1 && file.contains("removed") && file.at("removed").as_array().empty());
  std::vector<wal::FileChange> change;
  if (empty) change.push_back({wal::FileChange::Kind::kDelete, file_of(tenant, domain), ""});
  else change.push_back({wal::FileChange::Kind::kWrite, file_of(tenant, domain), json::pretty(file)});
  if (!wal.commit(change, error)) return false;
  gate.put(domain, std::make_shared<const controls::DomainControls>(std::move(*compiled)));
  return true;
}

// Where a control lives in the file: {"defaults"}, {"apis", api}, or {"endpoints", index}.
struct Place {
  std::string level;  // domain, api, endpoint
  std::string id;
  bj::object* entry = nullptr;
  size_t index = 0;
};
Place find_place(bj::object& file, const std::string& id) {
  if (id == "defaults") {
    auto* d = file.if_contains("defaults");
    return Place{"domain", id, d && d->is_object() ? &d->as_object() : nullptr, 0};
  }
  if (auto* apis = file.if_contains("apis"); apis && apis->is_object()) {
    if (auto* a = apis->as_object().if_contains(id); a && a->is_object()) return Place{"api", id, &a->as_object(), 0};
  }
  if (auto* eps = file.if_contains("endpoints"); eps && eps->is_array()) {
    auto& arr = eps->as_array();
    for (size_t i = 0; i < arr.size(); ++i) {
      if (!arr[i].is_object()) continue;
      if (const auto* e = arr[i].as_object().if_contains("id"); e && e->is_string() && e->as_string() == id) {
        return Place{"endpoint", id, &arr[i].as_object(), i};
      }
    }
  }
  return Place{"", id, nullptr, 0};
}

// What a box route answers, as a reader names it.
std::string route_what(const manifest::HttpRoute& r) {
  std::string verb = r.verb;
  for (auto& c : verb) c = char(std::toupper(static_cast<unsigned char>(c)));
  if (r.verb == "tool") return "TOOL /" + r.name;
  if (r.verb == "subscribe") return "subscription GET " + r.pattern.source;
  if (!r.pattern.source.empty()) return verb + " " + r.pattern.source;
  if (r.verb == "receive") return "mail " + r.mail_address;
  return r.verb + " " + r.name;
}

// Every route list of a manifest, with whether the request gate sees it (http, sse, and tools).
template <class F>
void each_route(const manifest::Manifest& m, F f) {
  for (const auto& r : m.routes) f(r, true);
  for (const auto& r : m.tools) f(r, true);
  for (const auto* list : {&m.timers, &m.internal, &m.identity, &m.mail, &m.results, &m.jobs, &m.upgrades}) {
    for (const auto& r : *list) f(r, false);
  }
}

}  // namespace

int64_t Host::control_timeout(const Request& req) {
  std::string v = req.header(kTimeoutHeader);
  if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) return 0;
  return std::stoll(v);
}

std::optional<int64_t> Host::control_fuel(const Request& req) {
  std::string v = req.header(kFuelHeader);
  if (v.empty() || v.size() > 12 || v.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
  return std::stoll(v);
}

bool Host::check_limits(const manifest::Manifest& m, std::string* error) {
  auto cfg = config_.get();
  bool ok = true;
  each_route(m, [&](const manifest::HttpRoute& r, bool) {
    if (!ok || !r.limits.any()) return;
    std::string err;
    if (!controls::parse_props(r.limits.set, limits_of(*cfg), "route " + r.name + " limits", &err)) {
      *error = err;
      ok = false;
    }
  });
  return ok;
}

bj::array Host::route_limits_json(const manifest::Manifest& m) {
  bj::array out;
  each_route(m, [&](const manifest::HttpRoute& r, bool gated) {
    if (!r.limits.any()) return;
    bj::object o{{"route", r.name}, {"what", route_what(r)}, {"id", controls::box_route_id(m.box, r.name)}, {"limits", r.limits.set}};
    if (!gated) o["note"] = "fuel only, enforced at each call; no client calls it, so nothing is counted";
    out.push_back(std::move(o));
  });
  return out;
}

std::shared_ptr<const Host::BoxLimits> Host::box_limits(const std::shared_ptr<const boxes::ActiveSet>& active) {
  auto cfg = config_.get();
  {
    std::lock_guard lock(box_limits_mu_);
    if (box_limits_ && box_limits_->set == active && box_limits_->config == cfg) return box_limits_;
  }
  // Compiled once per active set (an upload, a promotion, a disable) and per config (a reload
  // that lowers the host's limits), never per request.
  auto out = std::make_shared<BoxLimits>();
  out->set = active;
  out->config = cfg;
  for (const auto& [name, box] : active->boxes) {
    const auto& m = box->manifest;
    bool any = false;
    each_route(m, [&](const manifest::HttpRoute& r, bool gated) {
      if (!r.limits.any()) return;
      BoxLimits::Entry e{m.box, box->version, controls::box_route_id(m.box, r.name), route_what(r), &m, &r, gated, ""};
      std::string err;
      auto props = controls::parse_props(r.limits.set, limits_of(*cfg), "limits", &err);
      if (!props) {
        e.error = err;  // the host's own limits were lowered since the upload: the route runs without them
      } else if (gated) {
        out->routes.emplace(&r, controls::box_route_control(m.box, r.name, std::move(*props)));
        any = true;
      }
      out->entries.push_back(std::move(e));
    });
    if (!any) continue;
    for (const auto& d : m.domains) {
      if (d == "*") out->every_domain = true;
      else out->domains.insert(d);
    }
  }
  std::lock_guard lock(box_limits_mu_);
  box_limits_ = out;
  return out;
}

void Host::setup_controls() {
  if (!gate_) gate_ = std::make_unique<controls::Gate>([this](const std::string& k) { return address_hash_for_logs(k); });
  auto cfg = config_.get();
  std::lock_guard lock(controls_mu_);
  gate_->clear();
  for (const auto& [domain, entry] : cfg->domains) {
    bj::object file;
    if (auto text = base::read_file(wal_->root() / file_of(entry.tenant, domain))) {
      std::string err;
      auto v = json::parse(*text, &err);
      if (!v || !v->is_object()) {
        startup_notes_.push_back("controls for " + domain + " don't parse (" + err + "): only host defaults apply");
      } else {
        file = v->as_object();
      }
    }
    std::string err;
    auto compiled = controls::compile(file, domain, entry.tenant, cfg->controls_defaults, limits_of(*cfg), &err);
    if (!compiled) {
      startup_notes_.push_back("controls for " + domain + " refused (" + err + "): only host defaults apply");
      compiled = controls::compile({}, domain, entry.tenant, cfg->controls_defaults, limits_of(*cfg), &err);
    }
    if (compiled) gate_->put(domain, std::make_shared<const controls::DomainControls>(std::move(*compiled)));
  }
}

std::string Host::control_api(const Request& req, const router::NormalizedPath& path, const std::string& domain,
                              const manifest::HttpRoute** route) {
  if (route) *route = nullptr;
  const std::string& p = path.path;
  if (p == "/mcp") return "mcp";
  if (p.starts_with("/identity/") || p == "/oauth" || p.starts_with("/oauth/") || p.starts_with("/.well-known/") ||
      p.starts_with("/goober/connect/") || p.starts_with("/goober/list/") || p.starts_with("/goober/unsubscribe/") ||
      p.starts_with("/goober/mail/")) {
    return "identity";
  }
  if (p == "/attachment" || p.starts_with("/attachment/") || p.starts_with("/goober/pwa/") || p == site::kKeepFragmentPath) {
    return "content";
  }
  auto active = boxes_->active();
  const auto& table = active->table_for(domain);
  auto box_of = [&](const char* verb) -> std::string {
    auto m = table.match(path.segments, verb);
    if (!m.value) return "";
    if (route) *route = m.value->route;
    return "box:" + m.value->box->manifest.box;
  };
  if (req.method == "options") {
    // A preflight belongs to the box whose routes it asks about; its own limits don't apply.
    for (const char* v : {"get", "post", "put", "patch", "delete"}) {
      if (auto b = box_of(v); !b.empty()) {
        if (route) *route = nullptr;
        return b;
      }
    }
    return "content";
  }
  std::string verb = req.method == "head" ? "get" : req.method;
  if (verb == "get") {
    if (auto b = box_of("subscribe"); !b.empty()) return b;
  }
  if (auto b = box_of(verb.c_str()); !b.empty()) return b;
  return "content";
}

std::string Host::control_user_key(const Request& req, const std::string& tenant) {
  // Keyed by the credential itself (the bearer token, or the session cookie's value), never the
  // whole header: other cookies that change from request to request don't make a new entry, and
  // a request without a session's shape is nobody, with no lookup at all.
  std::string auth = req.header("authorization");
  std::string credential;
  Request probe;
  if (auth.size() > 7 && base::to_lower(auth.substr(0, 7)) == "bearer ") {
    credential = "b:" + auth.substr(7);
    probe.headers.emplace_back("authorization", auth);
  } else if (std::string session = session_cookie(req); !session.empty()) {
    credential = "s:" + session;
    probe.headers.emplace_back("cookie", session_cookie_name() + "=" + session);
  }
  if (credential.empty()) return "";
  std::string key = base::sha256_hex(tenant + "\n" + credential);
  int64_t now = base::now_unix_ms();
  {
    std::lock_guard lock(control_users_mu_);
    auto it = control_users_.find(key);
    if (it != control_users_.end() && now - it->second.first < kUserKeyMs) return it->second.second;
    std::string known = it == control_users_.end() ? std::string() : it->second.second;
    // Full: the stale, then the oldest few go, never every person at once.
    if (it == control_users_.end()) {
      base::make_room(control_users_, kUserKeysMax, now, kUserKeyMs, [](const auto& v) { return v.first; });
    }
    // Until the lookup lands, the request counts as its client's; one lookup at a time per credential.
    control_users_[key] = {now, known};
  }
  // Sessions and tokens are in SQLite: looked up on a worker, never on the network thread. The
  // lookup waits behind at most half the pool's queue; past that it isn't made, and the
  // request counts as its client's (the next one tries again).
  bool posted = pool_->post(
      [this, probe = std::move(probe), tenant, key] {
        std::string who;
        try {
          if (auto s = signed_in(probe, tenant)) who = (s->host ? "h:" : "u:") + std::to_string(s->user.user_id);
        } catch (const std::exception& e) {
          std::cerr << "goober-host: controls: who is signed in: " << e.what() << "\n";
        }
        std::lock_guard lock(control_users_mu_);
        control_users_[key] = {base::now_unix_ms(), who};
      },
      max_queued_ / 2);
  std::lock_guard lock(control_users_mu_);
  auto it = control_users_.find(key);
  if (!posted && it != control_users_.end() && it->second.second.empty()) {
    // Not looked up: forgotten, so the next request after the queue drains tries again.
    control_users_.erase(it);
    return "";
  }
  return it == control_users_.end() ? std::string() : it->second.second;
}

bool Host::control_gate(Request& req, Finish& finish, const std::string& domain, const std::string& tenant,
                        const router::NormalizedPath& path, int64_t started_us) {
  if (!gate_) return false;
  auto d = gate_->get(domain);
  // Box routes' own limits (§ 13): only where some live box with limits serves this domain.
  std::shared_ptr<const BoxLimits> boxes;
  if (auto active = boxes_->active(); active) {
    boxes = box_limits(active);
    if (boxes->routes.empty() || !boxes->applies(domain)) boxes.reset();
  }
  if (!d && !boxes) return false;
  controls::DomainControls none;
  if (!d) {
    none.domain = domain;
    none.tenant = tenant;
  }
  const controls::DomainControls& dc = d ? *d : none;
  const manifest::HttpRoute* route = nullptr;
  std::string api = dc.api_scoped || boxes ? control_api(req, path, domain, boxes ? &route : nullptr) : "";
  controls::Ask ask;
  ask.method = req.method;
  ask.path = &path.segments;
  ask.api = api;
  ask.client = rate_key(client_address(req));
  ask.user = [&] { return control_user_key(req, tenant); };
  ask.body_bytes = int64_t(req.body.size());
  ask.now_ms = base::now_unix_ms();
  ask.box = boxes && route ? boxes->find(route) : nullptr;
  auto v = gate_->check(dc, ask);
  if (!v.matched) return false;
  pulse_.add(ask.now_ms, operations::Pulse::kControlled);
  if (!v.would.empty()) pulse_.add(ask.now_ms, operations::Pulse::kControlWould);
  bj::array would;
  for (const auto& [id, reason] : v.would) would.emplace_back(id + ":" + controls::reason_name(reason));
  if (v.refused) {
    pulse_.add(ask.now_ms, operations::Pulse::kControlRefused);
    // The client is never told which control refused it (2026-11-03): a rate or concurrency
    // refusal says only "rate limit reached"; the access log keeps the control and its reason.
    bool limit = v.reason == controls::Reason::kRate || v.reason == controls::Reason::kConcurrent;
    std::string rid = req.header("goober-request-id");
    auto res = limit ? rate_limited(tenant, v.status, rid) : error_page(tenant, v.status, rid, v.message);
    if (v.retry_after_s >= 0) res.headers.emplace_back("retry-after", std::to_string(v.retry_after_s));
    bj::object note{{"id", v.control}, {"outcome", "refused"}, {"reason", controls::reason_name(v.reason)}};
    if (!would.empty()) note["would"] = std::move(would);
    if (!api.empty()) note["api"] = api;
    finish(std::move(res), json::dump(bj::object{{"why", "control refused"}, {"control", std::move(note)}}));
    return true;
  }
  if (v.timeout_ms > 0) req.headers.emplace_back(kTimeoutHeader, std::to_string(v.timeout_ms));
  if (v.fuel) req.headers.emplace_back(kFuelHeader, std::to_string(*v.fuel));
  bj::object note{{"id", v.id}, {"outcome", "allowed"}};
  if (!would.empty()) note["would"] = std::move(would);
  if (!api.empty()) note["api"] = api;
  // The ticket goes with the finish: done when it is called, released by its destructor when a
  // path never calls it.
  // A stream (an SSE subscription) holds it until the stream closes, so `concurrent` caps the
  // streams open at once, not only their opening; its latency is the time to open.
  finish = [inner = std::move(finish), ticket = std::move(v.ticket), started_us, text = json::dump(note)](
               Response res, std::string extra) {
    if (ticket && res.stream) {
      int64_t open_us = base::monotonic_us() - started_us;
      int status = res.status;
      res.stream = [open = std::move(res.stream), ticket, open_us, status](std::shared_ptr<server::http::Stream> s) {
        s->on_close([ticket, open_us, status] { ticket->done(status, open_us, base::now_unix_ms()); });
        open(std::move(s));
      };
    } else if (ticket) {
      ticket->done(res.status, base::monotonic_us() - started_us, base::now_unix_ms());
    }
    inner(std::move(res), with_control(extra, text));
  };
  return false;
}

Host::ToolGate Host::control_tool(const boxes::RouteRef& ref, const std::string& domain, const std::string& tenant,
                                  const SignedIn& who, const Request& req, int64_t input_bytes) {
  ToolGate out;
  // A control on /mcp may have put a fuel on the request; the tool's own gate may change it.
  out.fuel = control_fuel(req);
  if (!gate_) return out;
  auto d = gate_->get(domain);
  auto boxes = box_limits(boxes_->active());
  const controls::Control* box = boxes->find(ref.route);
  bool tool_overrides = false;
  if (d) {
    for (const auto& e : d->endpoints) tool_overrides |= e.match->method == "tool";
  }
  if (!box && !tool_overrides) return out;
  controls::DomainControls none;
  none.domain = domain;
  none.tenant = tenant;
  std::vector<std::string> path{ref.route->name};
  controls::Ask ask;
  ask.method = "tool";
  ask.path = &path;
  ask.client = rate_key(client_address(req));
  std::string user = (who.host ? "h:" : "u:") + std::to_string(who.user.user_id);
  ask.user = [user] { return user; };
  ask.body_bytes = input_bytes;
  ask.now_ms = base::now_unix_ms();
  ask.box = box;
  auto v = gate_->check(d ? *d : none, ask);
  if (!v.matched) return out;
  if (v.refused) {
    pulse_.add(ask.now_ms, operations::Pulse::kControlRefused);
    out.refused = true;
    out.status = v.status;
    bool limit = v.reason == controls::Reason::kRate || v.reason == controls::Reason::kConcurrent;
    out.message = limit ? "rate limit reached"
                  : v.reason == controls::Reason::kBody ? "the input is larger than this tool takes"
                  : v.message.empty() ? "this tool is closed" : v.message;
    return out;
  }
  if (v.fuel) out.fuel = v.fuel;
  out.ticket = std::move(v.ticket);
  return out;
}

ToolResult Host::control_set(const bj::object& args, const std::string& audit_action) {
  auto cfg = config_.get();
  std::string tenant, err;
  std::string domain = resolve_domain(*cfg, args, &tenant, &err);
  if (domain.empty()) return ToolResult::fail(err);
  std::string why = text_arg(args, "why").value_or("");
  if (why.empty()) return ToolResult::fail("why is required: say why the control changes");
  if (why.size() > controls::kWhyMaxBytes || !one_line(why)) return ToolResult::fail("why is one line of at most 300 bytes");
  auto api = text_arg(args, "api");
  if (args.contains("api") && !api) return ToolResult::fail("api is a string: box:<box name>, content, mcp, or identity");
  if (api && api->starts_with("box:") && api->find('/') != std::string::npos) {
    return ToolResult::fail("api: " + *api + " is a box route's own limits, from its manifest; beat them with an "
                            "endpoint override that matches the route (endpoint: {id, match})");
  }
  if (api && !controls::valid_api(*api, &err)) return ToolResult::fail("api: " + err);
  const bj::object* endpoint = nullptr;
  if (const auto* e = args.if_contains("endpoint")) {
    if (!e->is_object()) return ToolResult::fail("endpoint is an object: {id, match}");
    endpoint = &e->as_object();
    for (const auto& kv : *endpoint) {
      if (kv.key() != "id" && kv.key() != "match") {
        return ToolResult::fail("endpoint: unknown key " + std::string(kv.key()) + " (it takes id and match)");
      }
    }
  }
  std::string id;
  if (endpoint) {
    id = text_arg(*endpoint, "id").value_or("");
    if (!controls::valid_override_id(id, &err)) return ToolResult::fail("endpoint.id: " + err);
  } else {
    id = api ? *api : "defaults";
  }
  std::optional<std::string> mode = text_arg(args, "mode");
  if (args.contains("mode") && (!mode || (*mode != "enforce" && *mode != "observe"))) {
    return ToolResult::fail("mode is enforce or observe");
  }
  const bj::object* set = nullptr;
  if (const auto* s = args.if_contains("set")) {
    if (!s->is_object()) return ToolResult::fail("set is an object of properties, like {\"rate\": {\"per-minute\": 60}}");
    set = &s->as_object();
  }
  // Each property checked on its own first, so an error names it as the caller wrote it.
  if (set) {
    bj::object present;
    for (const auto& kv : *set) {
      if (!kv.value().is_null()) present[kv.key()] = kv.value();
    }
    if (!controls::parse_props(present, limits_of(*cfg), "set", &err)) return ToolResult::fail(err);
  }

  std::lock_guard lock(controls_mu_);
  bj::object file = load_file(*wal_, tenant, domain);
  auto place = find_place(file, id);
  std::string level = endpoint ? "endpoint" : api ? "api" : "domain";
  if (place.entry && place.level != level) {
    return ToolResult::fail(id + " is already a" + (place.level == "api" ? "n " : " ") + place.level + " control on " + domain);
  }
  bj::value before = entry_state(place.entry);
  bj::object entry = place.entry ? *place.entry : bj::object{};
  bool fresh = !place.entry;
  if (fresh && !mode) {
    return ToolResult::fail("mode is required for a new control: observe to watch what it would refuse first "
                            "(then control_report), or enforce to act now");
  }
  if (mode) entry["mode"] = *mode;
  bj::object props = entry.contains("set") && entry.at("set").is_object() ? entry.at("set").as_object() : bj::object{};
  if (set) {
    for (const auto& kv : *set) {
      if (kv.value().is_null()) props.erase(kv.key());
      else props[kv.key()] = kv.value();
    }
  }
  entry["set"] = props;
  if (endpoint) {
    entry["id"] = id;
    if (auto m = text_arg(*endpoint, "match")) {
      auto pat = controls::Pattern::parse(*m, &err);
      if (!pat) return ToolResult::fail("endpoint.match: " + err);
      if (pat->method == "tool" && api) return ToolResult::fail("a TOOL override matches tool calls on /mcp: it takes no api");
      entry["match"] = *m;
    } else if (fresh) {
      return ToolResult::fail("endpoint.match is required for a new override, like \"POST /cart/{id}\"");
    }
    if (api) entry["api"] = *api;
  }
  bj::value after = entry_state(&entry);
  bj::array history = entry.contains("history") && entry.at("history").is_array() ? entry.at("history").as_array() : bj::array{};
  std::string by = current_operator();
  history.push_back(bj::object{{"at_ms", base::now_unix_ms()}, {"by", by}, {"why", why}, {"before", before}, {"after", after}});
  while (history.size() > controls::kHistoryKept) history.erase(history.begin());
  entry["history"] = std::move(history);
  if (endpoint) {
    if (!file.contains("endpoints") || !file.at("endpoints").is_array()) file["endpoints"] = bj::array{};
    auto& eps = file["endpoints"].as_array();
    if (fresh) {
      if (eps.size() >= controls::kEndpointsMax) return ToolResult::fail("a domain has at most 100 endpoint overrides");
      eps.push_back(std::move(entry));
    } else {
      eps[place.index] = std::move(entry);
    }
  } else if (api) {
    if (!file.contains("apis") || !file.at("apis").is_object()) file["apis"] = bj::object{};
    file["apis"].as_object()[*api] = std::move(entry);
  } else {
    file["defaults"] = std::move(entry);
  }
  if (!keep_file(*cfg, *wal_, *gate_, tenant, domain, file, &err)) return ToolResult::fail("couldn't keep the control: " + err);
  audit(audit_action, by, "control:" + domain + "/" + id, "ok",
        bj::object{{"domain", domain}, {"tenant", tenant}, {"id", id}, {"before", before}, {"after", after}, {"why", why}});
  bj::object out{{"domain", domain}, {"tenant", tenant}, {"id", id},   {"level", level},
                 {"mode", after.at("mode")}, {"set", after.at("set")}, {"before", before}};
  if (auto d = gate_->get(domain)) {
    if (const auto* c = d->find(id)) {
      controls::DomainControls::Chain chain{d->host ? &*d->host : nullptr, d->defaults ? &*d->defaults : nullptr, nullptr,
                                            nullptr, nullptr};
      if (c->level == controls::Level::kApi) chain[controls::DomainControls::kAtApi] = c;
      if (c->level == controls::Level::kEndpoint) {
        if (!c->api.empty()) {
          if (auto it = d->apis.find(c->api); it != d->apis.end()) chain[controls::DomainControls::kAtApi] = &it->second;
        }
        if (c->match && c->match->method == "tool") chain = {nullptr, nullptr, nullptr, nullptr, nullptr};
        chain[controls::DomainControls::kAtEndpoint] = c;
      }
      out["effective"] = effective_json(chain);
    }
  }
  // Hints, not refusals: a box that isn't live here yet, and enforcing what was never watched.
  bj::array notes;
  if (api && api->starts_with("box:")) {
    auto active = boxes_->active();
    auto it = active->boxes.find(api->substr(4));
    bool serves = false;
    if (it != active->boxes.end()) {
      for (const auto& [tn, dm] : tenants_of(*cfg, it->second->manifest)) serves |= tn == tenant;
    }
    if (!serves) notes.emplace_back("no live box " + api->substr(4) + " serves " + domain + " yet: the control waits for it");
  }
  if (fresh && *mode == "enforce") notes.emplace_back("enforced at once: next time start in observe and read control_report first");
  if (!fresh && before.as_object().at("mode") == "observe" && after.at("mode") == "enforce") {
    notes.emplace_back("now enforced: control_report's effect compares before and after this change");
  }
  if (!notes.empty()) out["notes"] = std::move(notes);
  return ToolResult::ok(std::move(out));
}

ToolResult Host::control_remove(const bj::object& args, const std::string& audit_action) {
  auto cfg = config_.get();
  std::string tenant, err;
  std::string domain = resolve_domain(*cfg, args, &tenant, &err);
  if (domain.empty()) return ToolResult::fail(err);
  std::string id = text_arg(args, "id").value_or("");
  std::string why = text_arg(args, "why").value_or("");
  if (why.size() > controls::kWhyMaxBytes || !one_line(why)) return ToolResult::fail("why is one line of at most 300 bytes");
  if (id.starts_with("box:") && id.find('/') != std::string::npos) {
    return ToolResult::fail(id + " is a box route's own limits, from its manifest: a new version changes them, or an "
                            "endpoint override that matches the route beats them");
  }
  std::lock_guard lock(controls_mu_);
  bj::object file = load_file(*wal_, tenant, domain);
  auto place = find_place(file, id);
  if (!place.entry) {
    std::string ids;
    if (auto d = gate_->get(domain)) {
      if (d->defaults) ids += "defaults";
      for (const auto& [a, c] : d->apis) ids += (ids.empty() ? "" : ", ") + a;
      for (const auto& e : d->endpoints) ids += (ids.empty() ? "" : ", ") + e.id;
    }
    return ToolResult::fail("no control " + id + " on " + domain + (ids.empty() ? " (it has none)" : " (it has: " + ids + ")"));
  }
  bj::value last = entry_state(place.entry);
  bj::object gone = *place.entry;
  std::string by = current_operator();
  int64_t now = base::now_unix_ms();
  bj::array history = gone.contains("history") && gone.at("history").is_array() ? gone.at("history").as_array() : bj::array{};
  history.push_back(bj::object{{"at_ms", now}, {"by", by}, {"why", why}, {"before", last}, {"after", nullptr}});
  while (history.size() > controls::kHistoryKept) history.erase(history.begin());
  if (place.level == "domain") file.erase("defaults");
  else if (place.level == "api") {
    file["apis"].as_object().erase(id);
    if (file["apis"].as_object().empty()) file.erase("apis");
  } else {
    auto& eps = file["endpoints"].as_array();
    eps.erase(eps.begin() + std::ptrdiff_t(place.index));
    if (eps.empty()) file.erase("endpoints");
  }
  if (!file.contains("removed") || !file.at("removed").is_array()) file["removed"] = bj::array{};
  auto& removed = file["removed"].as_array();
  removed.push_back(bj::object{{"id", id}, {"level", place.level}, {"at_ms", now}, {"history", std::move(history)}});
  while (removed.size() > controls::kRemovedKept) removed.erase(removed.begin());
  if (!keep_file(*cfg, *wal_, *gate_, tenant, domain, file, &err)) return ToolResult::fail("couldn't keep the change: " + err);
  audit(audit_action, by, "control:" + domain + "/" + id, "ok",
        bj::object{{"domain", domain}, {"tenant", tenant}, {"id", id}, {"before", last}, {"why", why}});
  return ToolResult::ok(bj::object{{"domain", domain}, {"tenant", tenant}, {"id", id}, {"removed", last}});
}

std::pair<int, bj::object> Host::panel_control(const SignedIn& who, const std::string& what, const bj::object& args,
                                               const std::string& request_id) {
  // The human, 2026-11-03: "operators can override at any time since a control may be bum." A
  // person with host:owner or host:operator, in their scope; never held for approval.
  if (!may_change(who.roles)) {
    return {403, bj::object{{"error", "only host:owner or host:operator may change a control; host:observer looks"}}};
  }
  auto cfg = config_.get();
  std::string tenant, err;
  if (resolve_domain(*cfg, args, &tenant, &err).empty()) return {400, bj::object{{"error", err}}};
  auto scope = operator_scope("panel:" + who.user.email);
  if (!scope.all && !scope.tenants.count(tenant)) {
    return {403, bj::object{{"error", "you are focused on other tenants: " + tenant + " is outside your scope"}}};
  }
  if (what == "remove" && text_arg(args, "why").value_or("").empty()) {
    return {400, bj::object{{"error", "why is required: say why the control goes"}}};
  }
  ToolResult r;
  {
    OperatorContext as(*this, "panel:" + who.user.email, request_id);
    r = what == "set" ? control_set(args, "panel.control_set") : control_remove(args, "panel.control_remove");
  }
  if (r.is_error) return {400, bj::object{{"error", r.value}}};
  return {200, r.value.as_object()};
}

void Host::control_rows_to_traffic(int64_t now_ms) {
  if (gate_ && traffic_) traffic_->add_control_rows(gate_->take_closed(now_ms));
}

void Host::register_control_tools() {
  setup_controls();
  auto domain_prop = mcp::string_property("The domain, as domain_list names it (example.com): not a redirect.");

  mcp_->add_tool(
      {"control_set",
       "Changing tool. Sets one per-domain control (design/machine/047.Controls.md): the domain's defaults (no api, no "
       "endpoint), an API's control (api: box:<box name>, content, mcp, or identity), or an endpoint override "
       "(endpoint: {id, match}, match \"METHOD /path\" with {name} for one segment and {name*} for the rest; the most "
       "specific matching override wins; \"TOOL /name\" matches a box tool call on /mcp). set names only the "
       "properties it changes, each replacing that property whole; null removes one from this control, \"off\" lifts "
       "it here: rate {per-minute, burst, by: client|user|all} (429 + Retry-After), concurrent {max, per-client} (503 "
       "+ Retry-After: 1), body-max-bytes (413; only lower than the host's), timeout-ms (only lower than the host's), "
       "fuel (a box call's wasm fuel; only lower than the host's limits.fuel), closed {status: 503|410|451, "
       "retry-after-s, message} (answered without waking anything). Clients refused by rate or concurrent see only "
       "\"rate limit reached\". A box route's own limits (its manifest's limits) sit above an API's control and below "
       "an endpoint override: an override that matches the route beats them. mode is enforce or observe (required "
       "for a new control): an observed control refuses nothing and counts what it would have refused, while the "
       "enforced levels below it keep working. Start every new control in observe, read control_report, then set mode "
       "enforce. why is required, kept in the control's history, the audit log, and the change log. Takes effect on "
       "the next request; no restart. Answers id, level, mode, set, before, and effective (each property's value and "
       "where it comes from). A person may also set controls in the panel's Controls view.",
       mcp::object_schema(
           {{"domain", domain_prop},
            {"api", mcp::string_property("An API's control: box:<box name>, content, mcp, or identity. With endpoint, the "
                                         "override matches only that API's requests.")},
            {"endpoint", mcp::object_property("An endpoint override: {id, match}. id is yours (letters, digits, _); match is "
                                              "\"METHOD /path\" like \"POST /cart/{id}\", METHOD uppercase or *; GET covers "
                                              "HEAD; \"TOOL /search\" is the box tool search on /mcp. match is required "
                                              "when the override is new.")},
            {"set", mcp::object_property("The properties to change: rate, concurrent, body-max-bytes, timeout-ms, fuel, "
                                         "closed. null removes one from this control; \"off\" lifts it at this level.")},
            {"mode", mcp::enum_property("enforce or observe. Required for a new control: start in observe.", {"enforce", "observe"})},
            {"why", mcp::string_property("Why, in a sentence: kept with the control, in the audit log, and the change log.")}},
           {"domain", "why"}),
       [this](const bj::object& args) { return control_set(args, "control.set"); }});

  mcp_->add_tool(
      {"control_remove",
       "Changing tool. Removes one per-domain control by id: \"defaults\" (the domain's own defaults; host.json's "
       "controls.defaults stay), an API's name (box:<box name>, content, mcp, identity), or an endpoint override's id. "
       "A box route's own limits (box:<box>/<route>) come from its manifest and can't be removed here: an endpoint "
       "override beats them. Its rows stay in control_report until they age out (7 days), with its history. Audited, "
       "and in the change log. Takes effect on the next request.",
       mcp::object_schema({{"domain", domain_prop},
                           {"id", mcp::string_property("The control's id, as control_list shows it.")},
                           {"why", mcp::string_property("Why, in a sentence (optional): kept in the audit log and the history.")}},
                          {"domain", "id"}),
       [this](const bj::object& args) { return control_remove(args, "control.remove"); }});

  mcp_->add_tool(
      {"control_list",
       "Read tool. Per-domain controls (design/machine/047.Controls.md): for each domain (or the one given), host.json's "
       "controls.defaults, then each control (the domain's defaults, each API's, each box route's own limits from its "
       "manifest, each endpoint override, most specific first) with its id, level (domain, api, box, endpoint), api, "
       "match, mode, set (what it names itself), effective (each property's enforced value and where it comes from, "
       "and what observes it), in_flight now, and its last change. A box route's control has from: manifest, the box, "
       "its version, and what it answers; it is always enforced, and an endpoint override that matches the route "
       "beats it. An endpoint override's effective values leave out an API's control unless the override names its API.",
       mcp::object_schema({{"domain", domain_prop}}, {}),
       [this](const bj::object& args) {
         auto cfg = config_.get();
         std::vector<std::pair<std::string, std::string>> domains;
         if (args.contains("domain")) {
           std::string tenant, err;
           std::string d = resolve_domain(*cfg, args, &tenant, &err);
           if (d.empty()) return ToolResult::fail(err);
           domains.emplace_back(d, tenant);
         } else {
           for (const auto& [d, e] : cfg->domains) domains.emplace_back(d, e.tenant);
         }
         auto boxes = box_limits(boxes_->active());
         bj::array out;
         for (const auto& [domain, tenant] : domains) {
           auto d = gate_->get(domain);
           bj::object file = load_file(*wal_, tenant, domain);
           bj::object row{{"domain", domain}, {"tenant", tenant}};
           row["host_defaults"] = cfg->controls_defaults
                                      ? bj::value(bj::object{{"mode", controls::mode_name(cfg->controls_defaults->mode)},
                                                             {"set", controls::props_json(cfg->controls_defaults->props)}})
                                      : bj::value(nullptr);
           bj::array list;
           auto add = [&](const controls::Control& c, controls::DomainControls::Chain chain, const bj::object* entry) {
             bj::object o{{"id", c.id}, {"level", controls::level_name(c.level)}, {"mode", controls::mode_name(c.mode)},
                          {"set", controls::props_json(c.props)}, {"effective", effective_json(chain)},
                          {"in_flight", gate_->in_flight(domain, c.id)}};
             if (!c.api.empty()) o["api"] = c.api;
             if (c.match) o["match"] = c.match->text;
             auto h = history_of(entry);
             o["changes"] = int64_t(h.size());
             if (!h.empty()) {
               o["last_change"] = bj::object{{"at_ms", h.back().at_ms}, {"by", h.back().by}, {"why", h.back().why}};
             }
             list.push_back(std::move(o));
             return &list.back().as_object();
           };
           const controls::Control* host = d && d->host ? &*d->host : nullptr;
           const controls::Control* defaults = d && d->defaults ? &*d->defaults : nullptr;
           if (d) {
             if (d->defaults) {
               add(*d->defaults, {host, defaults, nullptr, nullptr, nullptr},
                   file.contains("defaults") ? &file.at("defaults").as_object() : nullptr);
             } else if (host) {
               add(*host, {host, nullptr, nullptr, nullptr, nullptr}, nullptr);  // host.json's, counted as "defaults"
             }
             for (const auto& [name, c] : d->apis) {
               const bj::object* entry = nullptr;
               if (auto* apis = file.if_contains("apis"); apis && apis->is_object()) {
                 if (auto* a = apis->as_object().if_contains(name); a && a->is_object()) entry = &a->as_object();
               }
               add(c, {host, defaults, &c, nullptr, nullptr}, entry);
             }
           }
           // The box routes' own limits, from the manifests of the boxes serving this domain.
           for (const auto& e : boxes->entries) {
             if (!e.manifest->applies_to(domain)) continue;
             bj::object* o = nullptr;
             if (const auto* c = boxes->find(e.route); c && e.gated) {
               const controls::Control* api = nullptr;
               if (d) {
                 if (auto it = d->apis.find(c->api); it != d->apis.end()) api = &it->second;
               }
               bool tool = e.route->verb == "tool";
               o = add(*c, tool ? controls::DomainControls::Chain{nullptr, nullptr, nullptr, c, nullptr}
                                : controls::DomainControls::Chain{host, defaults, api, c, nullptr},
                       nullptr);
             } else {
               list.push_back(bj::object{{"id", e.id}, {"level", "box"}, {"mode", "enforce"}, {"set", e.route->limits.set},
                                         {"api", "box:" + e.box}});
               o = &list.back().as_object();
               o->emplace("note", e.error.empty() ? "fuel only, enforced at each call; no client calls it, so nothing is counted"
                                                  : "not applied: " + e.error);
             }
             o->emplace("from", "manifest");
             o->emplace("box", e.box);
             o->emplace("version", e.version);
             o->emplace("what", e.what);
             o->emplace("limits", e.route->limits.set);
           }
           if (d) {
             for (const auto& c : d->endpoints) {
               const bj::object* entry = nullptr;
               if (auto* eps = file.if_contains("endpoints"); eps && eps->is_array()) {
                 for (const auto& e : eps->as_array()) {
                   if (e.is_object() && e.as_object().contains("id") && e.at("id") == bj::value(c.id)) entry = &e.as_object();
                 }
               }
               const controls::Control* api = nullptr;
               if (!c.api.empty()) {
                 if (auto it = d->apis.find(c.api); it != d->apis.end()) api = &it->second;
               }
               bool tool = c.match && c.match->method == "tool";
               add(c, tool ? controls::DomainControls::Chain{nullptr, nullptr, nullptr, nullptr, &c}
                           : controls::DomainControls::Chain{host, defaults, api, nullptr, &c},
                   entry);
             }
           }
           if (list.empty() && !cfg->controls_defaults && args.contains("domain") == false) continue;
           row["controls"] = std::move(list);
           if (const auto* r = file.if_contains("removed"); r && r->is_array() && !r->as_array().empty()) {
             bj::array removed;
             for (const auto& x : r->as_array()) {
               if (!x.is_object()) continue;
               removed.push_back(bj::object{{"id", x.as_object().contains("id") ? x.at("id") : bj::value(nullptr)},
                                            {"at_ms", x.as_object().contains("at_ms") ? x.at("at_ms") : bj::value(nullptr)}});
             }
             row["removed"] = std::move(removed);
           }
           out.push_back(std::move(row));
         }
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"control_report",
       "Read tool. What each per-domain control did, to judge whether it works: per five-minute point over window "
       "(1h, 12h, 7d), matched, allowed, refused (by reason: rate, concurrent, body, closed), would (what an observed "
       "control would have refused), peak in flight, p50/p99 latency and 5xx of the allowed; totals with distinct "
       "clients refused (approximate) and the busiest client's share; effect: equal spans before and after its last "
       "change (requests, refused, p99, 5xx share); its history; and verdicts in plain words (never matched; matched but "
       "never refused; refusals mostly from one client, shielding; refuses many clients, may be too tight; observe: "
       "would have refused N% from M clients). A box route's own limits (box:<box>/<route>, level box) are reported "
       "like the rest. Start a control in observe, read this, then enforce. Without id, every control of the domain, "
       "removed ones too while their rows last.",
       mcp::object_schema({{"domain", domain_prop},
                           {"id", mcp::string_property("One control's id, as control_list shows it. Omit for all.")},
                           {"window", mcp::enum_property("How far back: 1h, 12h (the default), or 7d.", {"1h", "12h", "7d"})}},
                          {"domain"}),
       [this](const bj::object& args) {
         auto cfg = config_.get();
         std::string tenant, err;
         std::string domain = resolve_domain(*cfg, args, &tenant, &err);
         if (domain.empty()) return ToolResult::fail(err);
         std::string window = text_arg(args, "window").value_or("12h");
         if (!controls::valid_report_window(window)) return ToolResult::fail("window is 1h, 12h, or 7d");
         auto only = text_arg(args, "id");
         int64_t now = base::now_unix_ms();
         std::vector<operations::ControlRow> rows;
         if (traffic_) {
           control_rows_to_traffic(now);
           traffic_->catch_up(now);
           rows = traffic_->control_rows(domain, only.value_or(""), now - 8 * 24 * 3600 * 1000LL, now + controls::kBucketMs);
         }
         for (auto& r : gate_->pending_rows(domain)) {
           if (!only || r.control == *only) rows.push_back(std::move(r));
         }
         std::map<std::string, controls::ReportInput> inputs;
         auto d = gate_->get(domain);
         bj::object file = load_file(*wal_, tenant, domain);
         auto configured = [&](const controls::Control& c, const bj::object* entry, const char* level) {
           auto& in = inputs[c.id];
           in.id = c.id;
           in.level = level;
           in.mode = c.mode;
           in.history = history_of(entry);
         };
         if (d) {
           if (d->defaults || d->host) {
             const auto& c = d->defaults ? *d->defaults : *d->host;
             configured(c, file.contains("defaults") ? &file.at("defaults").as_object() : nullptr, "domain");
             inputs["defaults"].id = "defaults";
           }
           for (const auto& [name, c] : d->apis) {
             const bj::object* entry = nullptr;
             if (auto* apis = file.if_contains("apis"); apis && apis->is_object()) {
               if (auto* a = apis->as_object().if_contains(name); a && a->is_object()) entry = &a->as_object();
             }
             configured(c, entry, "api");
           }
           for (const auto& c : d->endpoints) {
             const bj::object* entry = nullptr;
             if (auto* eps = file.if_contains("endpoints"); eps && eps->is_array()) {
               for (const auto& e : eps->as_array()) {
                 if (e.is_object() && e.as_object().contains("id") && e.at("id") == bj::value(c.id)) entry = &e.as_object();
               }
             }
             configured(c, entry, "endpoint");
           }
         }
         // Box routes' own limits: each its own monitored id, always enforced, with no history here
         // (version_diff is theirs).
         auto boxes = box_limits(boxes_->active());
         for (const auto& e : boxes->entries) {
           if (!e.gated || !e.manifest->applies_to(domain)) continue;
           if (const auto* c = boxes->find(e.route)) configured(*c, nullptr, "box");
         }
         // Removed controls: their history, while their rows last.
         if (const auto* r = file.if_contains("removed"); r && r->is_array()) {
           for (const auto& x : r->as_array()) {
             if (!x.is_object() || !x.as_object().contains("id") || !x.at("id").is_string()) continue;
             std::string id(x.at("id").as_string());
             if (inputs.count(id)) continue;
             auto& in = inputs[id];
             in.id = id;
             in.level = x.as_object().contains("level") && x.at("level").is_string() ? std::string(x.at("level").as_string()) : "";
             in.mode = std::nullopt;
             in.history = history_of(&x.as_object());
           }
         }
         for (auto& r : rows) {
           auto it = inputs.find(r.control);
           if (it == inputs.end()) {
             auto& in = inputs[r.control];
             in.id = r.control;
             in.mode = std::nullopt;
             it = inputs.find(r.control);
           }
           it->second.rows.push_back(std::move(r));
         }
         if (only && !inputs.count(*only)) return ToolResult::fail("no control " + *only + " on " + domain + ", and no rows of one");
         bj::array reports;
         for (auto& [id, in] : inputs) {
           if (only && id != *only) continue;
           // A removed control with no rows left has nothing to say.
           if (!in.mode && in.rows.empty() && !only) continue;
           reports.push_back(controls::report(in, window, now));
         }
         return ToolResult::ok(bj::object{{"domain", domain}, {"tenant", tenant}, {"window", window}, {"controls", std::move(reports)},
                                          {"mcp_queue", mcp_queue_report(domain, window)}});  // /mcp's queue (047 § 14)
       }});
}

}  // namespace goober::host
