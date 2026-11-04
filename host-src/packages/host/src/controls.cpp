// Per-domain controls (design/machine/047.Controls.md): what operators set per domain, API, and
// endpoint, kept as storage/controls/$tenant/$domain.json written through the host WAL (so the
// change log has every change, backups carry them, and a restart keeps them), compiled into the
// gate, checked on the request path, and reported per control. The matcher, the merge, and the
// counting are the controls package's; this file stores, wires, and serves them.

#include <algorithm>
#include <iostream>

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
  return controls::HostLimits{cfg.body_max_bytes, cfg.timeout_ms.max_value};
}

// Adds "control": note to an access line's extra (a JSON object's text, or "").
std::string with_control(const std::string& extra, const std::string& note) {
  if (extra.size() < 2 || extra.front() != '{' || extra.back() != '}') return "{\"control\":" + note + "}";
  if (extra == "{}") return "{\"control\":" + note + "}";
  return extra.substr(0, extra.size() - 1) + ",\"control\":" + note + "}";
}

// Each property along a chain: where its enforced value comes from, and what observes it.
bj::object effective_json(const controls::DomainControls::Chain& chain) {
  bj::object enforced, observed;
  for (auto p : controls::kProperties) {
    auto e = controls::effective(chain, p);
    auto from = [](const controls::Control& c) {
      return c.level == controls::Level::kHost ? std::string("host.json controls.defaults") : c.id;
    };
    if (e.enforced) {
      enforced[controls::property_name(p)] =
          bj::object{{"value", controls::property_json(e.enforced->props, p)}, {"from", from(*e.enforced)}};
    }
    if (e.observed) {
      observed[controls::property_name(p)] =
          bj::object{{"value", controls::property_json(e.observed->props, p)}, {"from", from(*e.observed)}};
    }
  }
  return bj::object{{"enforced", std::move(enforced)}, {"observed", std::move(observed)}};
}

}  // namespace

int64_t Host::control_timeout(const Request& req) {
  std::string v = req.header(kTimeoutHeader);
  if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) return 0;
  return std::stoll(v);
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

std::string Host::control_api(const Request& req, const router::NormalizedPath& path, const std::string& domain) {
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
    return m.value ? "box:" + m.value->box->manifest.box : "";
  };
  if (req.method == "options") {
    // A preflight belongs to the box whose routes it asks about.
    for (const char* v : {"get", "post", "put", "patch", "delete"}) {
      if (auto b = box_of(v); !b.empty()) return b;
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
  std::string auth = req.header("authorization");
  std::string credential;
  if (auth.size() > 7 && base::to_lower(auth.substr(0, 7)) == "bearer ") {
    credential = "b:" + auth.substr(7);
  } else if (std::string cookie = req.header("cookie"); !cookie.empty()) {
    credential = "c:" + cookie;
  }
  if (credential.empty()) return "";
  std::string key = base::sha256_hex(tenant + "\n" + credential);
  int64_t now = base::now_unix_ms();
  {
    std::lock_guard lock(control_users_mu_);
    auto it = control_users_.find(key);
    if (it != control_users_.end() && now - it->second.first < kUserKeyMs) return it->second.second;
    if (control_users_.size() >= kUserKeysMax) control_users_.clear();
    // Until the lookup lands, the request counts as its client's; one lookup at a time per credential.
    control_users_[key] = {now, it == control_users_.end() ? std::string() : it->second.second};
  }
  // Sessions and tokens are in SQLite: looked up on a worker, never on the network thread.
  Request probe;
  for (const char* h : {"authorization", "cookie"}) {
    if (auto v = req.header(h); !v.empty()) probe.headers.emplace_back(h, v);
  }
  pool_->post([this, probe = std::move(probe), tenant, key] {
    std::string who;
    try {
      if (auto s = signed_in(probe, tenant)) who = (s->host ? "h:" : "u:") + std::to_string(s->user.user_id);
    } catch (const std::exception& e) {
      std::cerr << "goober-host: controls: who is signed in: " << e.what() << "\n";
    }
    std::lock_guard lock(control_users_mu_);
    control_users_[key] = {base::now_unix_ms(), who};
  });
  std::lock_guard lock(control_users_mu_);
  return control_users_[key].second;
}

bool Host::control_gate(Request& req, Finish& finish, const std::string& domain, const std::string& tenant,
                        const router::NormalizedPath& path, int64_t started_us) {
  if (!gate_) return false;
  auto d = gate_->get(domain);
  if (!d) return false;
  std::string api = d->api_scoped ? control_api(req, path, domain) : "";
  controls::Ask ask;
  ask.method = req.method;
  ask.path = &path.segments;
  ask.api = api;
  ask.client = rate_key(client_address(req));
  ask.user = [&] { return control_user_key(req, tenant); };
  ask.body_bytes = int64_t(req.body.size());
  ask.now_ms = base::now_unix_ms();
  auto v = gate_->check(*d, ask);
  if (!v.matched) return false;
  pulse_.add(ask.now_ms, operations::Pulse::kControlled);
  if (!v.would.empty()) pulse_.add(ask.now_ms, operations::Pulse::kControlWould);
  bj::array would;
  for (const auto& [id, reason] : v.would) would.emplace_back(id + ":" + controls::reason_name(reason));
  if (v.refused) {
    pulse_.add(ask.now_ms, operations::Pulse::kControlRefused);
    auto res = error_page(tenant, v.status, req.header("goober-request-id"), v.message);
    if (v.retry_after_s >= 0) res.headers.emplace_back("retry-after", std::to_string(v.retry_after_s));
    bj::object note{{"id", v.control}, {"outcome", "refused"}, {"reason", controls::reason_name(v.reason)}};
    if (!would.empty()) note["would"] = std::move(would);
    if (!api.empty()) note["api"] = api;
    finish(std::move(res), json::dump(bj::object{{"why", "control refused"}, {"control", std::move(note)}}));
    return true;
  }
  if (v.timeout_ms > 0) req.headers.emplace_back(kTimeoutHeader, std::to_string(v.timeout_ms));
  bj::object note{{"id", v.id}, {"outcome", "allowed"}};
  if (!would.empty()) note["would"] = std::move(would);
  if (!api.empty()) note["api"] = api;
  // The ticket goes with the finish: done when it is called, released by its destructor when a
  // path never calls it.
  finish = [inner = std::move(finish), ticket = std::move(v.ticket), started_us, text = json::dump(note)](
               Response res, std::string extra) {
    if (ticket) ticket->done(res.status, base::monotonic_us() - started_us, base::now_unix_ms());
    inner(std::move(res), with_control(extra, text));
  };
  return false;
}

void Host::control_rows_to_traffic(int64_t now_ms) {
  if (gate_ && traffic_) traffic_->add_control_rows(gate_->take_closed(now_ms));
}

void Host::register_control_tools() {
  setup_controls();
  auto domain_prop = mcp::string_property("The domain, as domain_list names it (example.com): not a redirect.");

  // The configured domain and its tenant, or why not.
  auto resolve = [this](const bj::object& args, std::string* tenant, std::string* error) -> std::string {
    auto cfg = config_.get();
    std::string d = base::to_lower(text_arg(args, "domain").value_or(""));
    if (d.empty()) return *error = "domain is required", "";
    if (auto r = cfg->redirects.find(d); r != cfg->redirects.end()) {
      return *error = d + " redirects to " + r->second + ": set its controls there", "";
    }
    auto it = cfg->domains.find(d);
    if (it == cfg->domains.end()) return *error = "no domain " + d + " on this host (domain_list names them)", "";
    *tenant = it->second.tenant;
    return d;
  };
  auto load = [this](const std::string& tenant, const std::string& domain) {
    bj::object file;
    if (auto text = base::read_file(wal_->root() / file_of(tenant, domain))) {
      std::string err;
      if (auto v = json::parse(*text, &err); v && v->is_object()) file = v->as_object();
    }
    return file;
  };
  // Checks the whole file, writes it through the WAL, and puts it in the gate: the next request sees it.
  auto keep = [this](const std::string& tenant, const std::string& domain, const bj::object& file, std::string* error) {
    auto cfg = config_.get();
    auto compiled = controls::compile(file, domain, tenant, cfg->controls_defaults, limits_of(*cfg), error);
    if (!compiled) return false;
    bool empty = file.empty() || (file.size() == 1 && file.contains("removed") && file.at("removed").as_array().empty());
    std::vector<wal::FileChange> change;
    if (empty) change.push_back({wal::FileChange::Kind::kDelete, file_of(tenant, domain), ""});
    else change.push_back({wal::FileChange::Kind::kWrite, file_of(tenant, domain), json::pretty(file)});
    if (!wal_->commit(change, error)) return false;
    gate_->put(domain, std::make_shared<const controls::DomainControls>(std::move(*compiled)));
    return true;
  };
  // Where a control lives in the file: {"defaults"}, {"apis", api}, or {"endpoints", index}.
  struct Place {
    std::string level;  // domain, api, endpoint
    std::string id;
    bj::object* entry = nullptr;
    size_t index = 0;
  };
  auto find = [](bj::object& file, const std::string& id) -> Place {
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
  };

  mcp_->add_tool(
      {"control_set",
       "Changing tool. Sets one per-domain control (design/machine/047.Controls.md): the domain's defaults (no api, no "
       "endpoint), an API's control (api: box:<box name>, content, mcp, or identity), or an endpoint override "
       "(endpoint: {id, match}, match \"METHOD /path\" with {name} for one segment and {name*} for the rest; the most "
       "specific matching override wins). set names only the properties it changes, each replacing that property "
       "whole; null removes one from this control, \"off\" lifts it here: rate {per-minute, burst, by: client|user|all} "
       "(429 + Retry-After), concurrent {max, per-client} (503 + Retry-After: 1), body-max-bytes (413; only lower than "
       "the host's), timeout-ms (only lower than the host's), closed {status: 503|410|451, retry-after-s, message} "
       "(answered without waking anything). mode is enforce or observe (required for a new control): an observed "
       "control refuses nothing and counts what it would have refused, while the enforced levels below it keep "
       "working. Start every new control in observe, read control_report, then set mode enforce. why is required, "
       "kept in the control's history, the audit log, and the change log. Takes effect on the next request; no "
       "restart. Answers id, level, mode, set, before, and effective (each property's value and where it comes from).",
       mcp::object_schema(
           {{"domain", domain_prop},
            {"api", mcp::string_property("An API's control: box:<box name>, content, mcp, or identity. With endpoint, the "
                                         "override matches only that API's requests.")},
            {"endpoint", mcp::object_property("An endpoint override: {id, match}. id is yours (letters, digits, _); match is "
                                              "\"METHOD /path\" like \"POST /cart/{id}\", METHOD uppercase or *; GET covers "
                                              "HEAD. match is required when the override is new.")},
            {"set", mcp::object_property("The properties to change: rate, concurrent, body-max-bytes, timeout-ms, closed. "
                                         "null removes one from this control; \"off\" lifts it at this level.")},
            {"mode", mcp::enum_property("enforce or observe. Required for a new control: start in observe.", {"enforce", "observe"})},
            {"why", mcp::string_property("Why, in a sentence: kept with the control, in the audit log, and the change log.")}},
           {"domain", "why"}),
       [this, resolve, load, keep, find](const bj::object& args) {
         std::string tenant, err;
         std::string domain = resolve(args, &tenant, &err);
         if (domain.empty()) return ToolResult::fail(err);
         std::string why = text_arg(args, "why").value_or("");
         if (why.empty()) return ToolResult::fail("why is required: say why the control changes");
         if (why.size() > controls::kWhyMaxBytes || !one_line(why)) return ToolResult::fail("why is one line of at most 300 bytes");
         auto api = text_arg(args, "api");
         if (args.contains("api") && !api) return ToolResult::fail("api is a string: box:<box name>, content, mcp, or identity");
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
         auto cfg = config_.get();
         // Each property checked on its own first, so an error names it as the caller wrote it.
         if (set) {
           bj::object present;
           for (const auto& kv : *set) {
             if (!kv.value().is_null()) present[kv.key()] = kv.value();
           }
           if (!controls::parse_props(present, limits_of(*cfg), "set", &err)) return ToolResult::fail(err);
         }

         std::lock_guard lock(controls_mu_);
         bj::object file = load(tenant, domain);
         auto place = find(file, id);
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
         if (!keep(tenant, domain, file, &err)) return ToolResult::fail("couldn't keep the control: " + err);
         audit("control.set", by, "control:" + domain + "/" + id, "ok",
               bj::object{{"domain", domain}, {"tenant", tenant}, {"id", id}, {"before", before}, {"after", after}, {"why", why}});
         bj::object out{{"domain", domain}, {"tenant", tenant}, {"id", id},   {"level", level},
                        {"mode", after.at("mode")}, {"set", after.at("set")}, {"before", before}};
         if (auto d = gate_->get(domain)) {
           if (const auto* c = d->find(id)) {
             controls::DomainControls::Chain chain{d->host ? &*d->host : nullptr, d->defaults ? &*d->defaults : nullptr, nullptr, nullptr};
             if (c->level == controls::Level::kApi) chain[2] = c;
             if (c->level == controls::Level::kEndpoint) {
               if (!c->api.empty()) {
                 if (auto it = d->apis.find(c->api); it != d->apis.end()) chain[2] = &it->second;
               }
               chain[3] = c;
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
       }});

  mcp_->add_tool(
      {"control_remove",
       "Changing tool. Removes one per-domain control by id: \"defaults\" (the domain's own defaults; host.json's "
       "controls.defaults stay), an API's name (box:<box name>, content, mcp, identity), or an endpoint override's id. "
       "Its rows stay in control_report until they age out (7 days), with its history. Audited, and in the change log. "
       "Takes effect on the next request.",
       mcp::object_schema({{"domain", domain_prop},
                           {"id", mcp::string_property("The control's id, as control_list shows it.")},
                           {"why", mcp::string_property("Why, in a sentence (optional): kept in the audit log and the history.")}},
                          {"domain", "id"}),
       [this, resolve, load, keep, find](const bj::object& args) {
         std::string tenant, err;
         std::string domain = resolve(args, &tenant, &err);
         if (domain.empty()) return ToolResult::fail(err);
         std::string id = text_arg(args, "id").value_or("");
         std::string why = text_arg(args, "why").value_or("");
         if (why.size() > controls::kWhyMaxBytes || !one_line(why)) return ToolResult::fail("why is one line of at most 300 bytes");
         std::lock_guard lock(controls_mu_);
         bj::object file = load(tenant, domain);
         auto place = find(file, id);
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
         if (!keep(tenant, domain, file, &err)) return ToolResult::fail("couldn't keep the change: " + err);
         audit("control.remove", by, "control:" + domain + "/" + id, "ok",
               bj::object{{"domain", domain}, {"tenant", tenant}, {"id", id}, {"before", last}, {"why", why}});
         return ToolResult::ok(bj::object{{"domain", domain}, {"tenant", tenant}, {"id", id}, {"removed", last}});
       }});

  mcp_->add_tool(
      {"control_list",
       "Read tool. Per-domain controls (design/machine/047.Controls.md): for each domain (or the one given), host.json's "
       "controls.defaults, then each control (the domain's defaults, each API's, each endpoint override, most specific "
       "first) with its id, level, api, match, mode, set (what it names itself), effective (each property's enforced "
       "value and where it comes from, and what observes it), in_flight now, and its last change. An endpoint "
       "override's effective values leave out an API's control unless the override names its API.",
       mcp::object_schema({{"domain", domain_prop}}, {}),
       [this, resolve, load](const bj::object& args) {
         auto cfg = config_.get();
         std::vector<std::pair<std::string, std::string>> domains;
         if (args.contains("domain")) {
           std::string tenant, err;
           std::string d = resolve(args, &tenant, &err);
           if (d.empty()) return ToolResult::fail(err);
           domains.emplace_back(d, tenant);
         } else {
           for (const auto& [d, e] : cfg->domains) domains.emplace_back(d, e.tenant);
         }
         bj::array out;
         for (const auto& [domain, tenant] : domains) {
           auto d = gate_->get(domain);
           bj::object file = load(tenant, domain);
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
           };
           if (d) {
             const controls::Control* host = d->host ? &*d->host : nullptr;
             if (d->defaults) {
               add(*d->defaults, {host, &*d->defaults, nullptr, nullptr},
                   file.contains("defaults") ? &file.at("defaults").as_object() : nullptr);
             } else if (host) {
               add(*host, {host, nullptr, nullptr, nullptr}, nullptr);  // host.json's, counted as "defaults"
             }
             for (const auto& [name, c] : d->apis) {
               const bj::object* entry = nullptr;
               if (auto* apis = file.if_contains("apis"); apis && apis->is_object()) {
                 if (auto* a = apis->as_object().if_contains(name); a && a->is_object()) entry = &a->as_object();
               }
               add(c, {host, d->defaults ? &*d->defaults : nullptr, &c, nullptr}, entry);
             }
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
               add(c, {host, d->defaults ? &*d->defaults : nullptr, api, &c}, entry);
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
       "would have refused N% from M clients). Start a control in observe, read this, then enforce. Without id, every "
       "control of the domain, removed ones too while their rows last.",
       mcp::object_schema({{"domain", domain_prop},
                           {"id", mcp::string_property("One control's id, as control_list shows it. Omit for all.")},
                           {"window", mcp::enum_property("How far back: 1h, 12h (the default), or 7d.", {"1h", "12h", "7d"})}},
                          {"domain"}),
       [this, resolve, load](const bj::object& args) {
         std::string tenant, err;
         std::string domain = resolve(args, &tenant, &err);
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
         bj::object file = load(tenant, domain);
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
                                          {"mcp_queue", mcp_queue_report(domain, window)}});  // /mcp's queue (047 § 13)
       }});
}

}  // namespace goober::host
