// The operator panel (design/machine/028.Panel.md): its own origin (operations.domain, or a
// listener of its own), host-realm sign-in, static assets compiled into the binary, and
// POST /api/$tool for the read tools. Also the tools only the panel needed until now:
// traffic, hot_boxes, box_graph, changes, change_get, and operator_actions.

#include "build_id.h"

#include <iostream>
#include <map>
#include <set>

#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/calendar/zones.h"
#include "goober/libraries/libraries.h"
#include "goober/operations/changes.h"
#include "goober/operations/traffic.h"
#include "goober/panel/assets.h"
#include "grant_usage.h"
#include "http_util.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

namespace {

// What host:observer and the panel may call (§ 7): tools that read and change nothing.
const std::set<std::string, std::less<>> kReadTools = {
    "host_health",     "traffic",           "hot_boxes",        "box_graph",       "access_log",
    "changes",         "change_get",        "operator_actions", "box_list",        "box_logs",
    "version_list",    "version_get",       "route_check",      "mcp_tools_preview", "timers_list",
    "outbox_dead_letters", "db_list",       "db_describe",      "db_query",        "schema_list",
    "schema_get",      "domain_list",       "realm_get",        "client_list",     "content_list",
    "content_get",     "page_list",         "page_get",         "page_render",     "template_list",   "template_get",
    "partial_list",    "partial_get",       "attachment_list",  "attachment_get",  "mail_activity",
    "mail_unsubscribed", "mail_template_list", "mail_template_get", "mail_template_preview", "source_list",
    "source_read",     "source_log",        "source_diff",      "source_job",      "backup_status",
    "key_list",        "crawl_list",        "crawl_pages",      "crawl_records",   "box_guide",
    "site_guide",      "rust_binding",      "pwa_get",          "pwa_list",        "pwa_subscriptions",
    "library_list",    "library_get",       "operator_scope",   "invite_list",     "invite_get",
    "mail_preferences", "mail_lists",       "list_etags",       "url_check",       "page_lint",
    "approval_verify", "approval_list",     "tool_schemas",     "log_search",      "log_verify",
    "log_status",      "uri_stats",         "uri_tree",         "uri_flow",        "agent_audit",
    "spec_list",       "spec_get",          "approval_status",  "bus_status",      "box_render",
    "source_tree",     "source_box_history", "grant_usage"};

const std::set<std::string, std::less<>> kPanelRoles = {"host:owner", "host:operator", "host:observer"};

constexpr const char* kPanelCsp =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; "
    "frame-ancestors 'none'; base-uri 'none'; form-action 'self'";

void secure_headers(Response& res) {
  res.headers.emplace_back("content-security-policy", kPanelCsp);
  res.headers.emplace_back("x-frame-options", "DENY");
  res.headers.emplace_back("x-content-type-options", "nosniff");
  res.headers.emplace_back("referrer-policy", "same-origin");
}

Response json_reply(int status, const bj::value& body) {
  Response res;
  res.status = status;
  res.headers.emplace_back("content-type", "application/json");
  res.headers.emplace_back("cache-control", "no-store");
  secure_headers(res);
  res.body = json::dump(body);
  return res;
}

std::string arg_text(const bj::object& args, const char* k) {
  auto* v = args.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

int64_t arg_int(const bj::object& args, const char* k, int64_t fallback) {
  auto* v = args.if_contains(k);
  if (v && v->is_int64()) return v->as_int64();
  if (v && v->is_uint64()) return int64_t(v->as_uint64());
  return fallback;
}

}  // namespace

bool Host::is_read_tool(std::string_view tool) { return kReadTools.count(tool) > 0; }

std::string& Host::current_request_id() {
  thread_local std::string id;
  return id;
}

std::vector<std::string> Host::operator_roles(const std::string& operator_name) const {
  // The config's token and minted tokens are the owner's own (design/machine/023 § 3).
  if (operator_name == "config" || operator_name.starts_with("token:")) return {"host:owner"};
  if (operator_name.starts_with("oauth:")) {
    std::string rest = operator_name.substr(6);
    std::string email = rest.substr(0, rest.rfind(':'));
    auto cfg = config_.get();
    if (auto it = cfg->identity.operators.find(email); it != cfg->identity.operators.end()) return it->second;
  }
  return {};
}

bool Host::may_change(const std::vector<std::string>& roles) {
  for (const auto& r : roles) {
    if (r == "host:owner" || r == "host:operator") return true;
  }
  return false;
}

bool Host::is_panel_request(const Request& req, bool panel_listener) const {
  if (panel_listener) return true;
  std::string domain = config_.get()->panel_domain();
  if (domain.empty()) return false;
  std::string h = base::to_lower(req.header("host"));
  if (auto colon = h.rfind(':'); colon != std::string::npos && h.find(']') == std::string::npos) h = h.substr(0, colon);
  if (!h.empty() && h.back() == '.') h.pop_back();
  return h == domain;
}

void Host::handle_panel(const Request& req, const Finish& inner, const std::string& path, const std::string& domain) {
  // Every line of the panel's own traffic says so, for the rollups (§ 4).
  Finish finish = [inner](Response res, std::string extra) {
    std::string err;
    bj::object fields;
    if (auto v = json::parse(extra, &err); v && v->is_object()) fields = v->as_object();
    if (!fields.contains("panel")) fields["panel"] = "page";
    inner(std::move(res), json::dump(fields));
  };
  std::string request_id = req.header("goober-request-id");
  bool is_get = req.method == "get" || req.method == "head";

  if (path.starts_with("/identity/")) {
    return handle_identity(req, finish, path, domain, std::string(identity::kHostRealm), true);
  }
  auto who = browser_session(req, std::string(identity::kHostRealm));
  bool allowed = false;
  if (who && who->host) {
    for (const auto& r : who->roles) allowed |= kPanelRoles.count(r) > 0;
  }

  if (is_get && path != "/api/me" && !path.starts_with("/api/")) {
    const auto* asset = panel::find(path);
    if (!asset) return finish(json_reply(404, bj::object{{"error", "not found"}}), R"({"why":"no such panel path"})");
    // The page itself sends a signed-out browser to sign in; the script and styles hold no data.
    if (path == "/" && !who) {
      Response res;
      res.status = 303;
      res.headers.emplace_back("location", "/identity/sign-in/as-host?return=%2F");
      res.headers.emplace_back("cache-control", "no-store");
      return finish(std::move(res), "");
    }
    if (path == "/" && !allowed) {
      return finish(json_reply(403, bj::object{{"error", "the panel needs host:owner, host:operator, or host:observer"}}),
                    R"({"why":"no panel role"})");
    }
    Response res;
    res.headers.emplace_back("content-type", std::string(asset->content_type));
    res.headers.emplace_back("etag", std::string(asset->etag));
    res.headers.emplace_back("cache-control", path == "/" ? "no-cache" : "public, max-age=300, must-revalidate");
    secure_headers(res);
    if (path != "/" && req.header("if-none-match") == asset->etag) {
      res.status = 304;
    } else {
      res.body = std::string(asset->body);
    }
    res.head_only = req.method == "head";
    return finish(std::move(res), "");
  }

  if (path == "/api/me" || path.starts_with("/api/")) {
    if (!who) return finish(json_reply(401, bj::object{{"error", "sign in as a host operator"}}), R"({"why":"signed out"})");
    if (!allowed) {
      return finish(json_reply(403, bj::object{{"error", "the panel needs host:owner, host:operator, or host:observer"}}),
                    R"({"why":"no panel role"})");
    }
    if (path == "/api/me") {
      bj::array roles;
      for (const auto& r : who->roles) roles.push_back(bj::value(r));
      auto cfg = config_.get();
      bj::array tenants;
      std::set<std::string> seen;
      for (const auto& [d, e] : cfg->domains) {
        if (seen.insert(e.tenant).second) tenants.push_back(bj::value(e.tenant));
      }
      auto scope = operator_scope("panel:" + who->user.email);
      bool owner = std::find(who->roles.begin(), who->roles.end(), "host:owner") != who->roles.end();
      bj::array all_tenants = tenants;
      bj::value focus = nullptr;
      if (!scope.all) {
        bj::array ts;
        for (const auto& t : scope.tenants) ts.push_back(bj::value(t));
        focus = std::move(ts);
        // The tenant pickers offer only the scope.
        bj::array kept;
        for (const auto& t : tenants) {
          if (scope.tenants.count(std::string(t.as_string()))) kept.push_back(t);
        }
        tenants = std::move(kept);
      }
      return finish(json_reply(200, bj::object{{"email", who->user.email},
                                               {"roles", std::move(roles)},
                                               {"focus", std::move(focus)},
                                               {"all_tenants", owner ? std::move(all_tenants) : bj::array(tenants)},
                                               {"build", std::string(kBuildCommit)},
                                               {"tenants", std::move(tenants)},
                                               {"domain", domain}}),
                    R"({"panel":"me"})");
    }
    std::string tool = path.substr(5);
    if (req.method != "post") {
      auto res = json_reply(405, bj::object{{"error", "POST a JSON object of the tool's arguments"}});
      res.headers.emplace_back("allow", "POST");
      return finish(std::move(res), "");
    }
    // A cross-site page can't make a browser send JSON with the cookie without a preflight,
    // and the Origin must be this one (§ 2).
    std::string type = base::to_lower(req.header("content-type"));
    if (!type.starts_with("application/json")) {
      return finish(json_reply(415, bj::object{{"error", "content-type must be application/json"}}),
                    R"({"why":"panel content type"})");
    }
    if (!same_origin(req, origin_of(req))) {
      return finish(json_reply(403, bj::object{{"error", "cross-origin"}}), R"({"why":"panel cross-origin"})");
    }
    std::string err;
    bj::object args;
    if (!req.body.empty()) {
      auto v = json::parse(req.body, &err);
      if (!v || !v->is_object()) {
        return finish(json_reply(400, bj::object{{"error", "the body must be a JSON object"}}), "");
      }
      args = v->as_object();
    }
    std::string me = "panel:" + who->user.email;
    // The panel's own writes: the only changes it makes, each a human's (030 § 2, 029 § 6).
    // The panel's own answers are {"result": ...}, as a tool's are, which is what its script reads.
    auto own = [](int status, bj::object body) {
      return status == 200 ? json_reply(200, bj::object{{"result", std::move(body)}}) : json_reply(status, body);
    };
    if (tool == "operators") return finish(own(200, panel_operators(*who)), R"({"panel":"operators"})");
    if (tool == "approvals") return finish(own(200, panel_approvals(*who)), R"({"panel":"approvals"})");
    // Approvals (035): turning them on or off for an agent, and deciding a held call.
    if (tool == "write/approvals" || tool == "write/decide") {
      auto [status, body] = tool == "write/approvals" ? panel_set_approvals(*who, args, request_id) : panel_decide(*who, args, request_id);
      audit(tool == "write/approvals" ? "panel.approvals_set" : "panel.decide", me,
            tool == "write/approvals" ? arg_text(args, "agent") : std::to_string(args.contains("id") && args.at("id").is_int64() ? args.at("id").as_int64() : 0),
            status == 200 ? "ok" : "refused", bj::object{{"ip", client_address(req)}, {"request_id", request_id}});
      return finish(own(status, body), json::dump(bj::object{{"panel", tool}, {"operator", me}}));
    }
    if (tool == "write/scope" || tool == "write/unblock") {
      auto [status, body] = tool == "write/scope" ? panel_set_scope(*who, args, request_id)
                                                  : panel_unblock(*who, args, request_id);
      audit(tool == "write/scope" ? "panel.scope_set" : "panel.block_lift", me,
            tool == "write/scope" ? arg_text(args, "operator") : arg_text(args, "tenant"), status == 200 ? "ok" : "refused",
            bj::object{{"ip", client_address(req)}, {"request_id", request_id}});
      return finish(own(status, body), json::dump(bj::object{{"panel", tool}, {"operator", me}}));
    }
    if (!is_read_tool(tool)) {
      return finish(json_reply(403, bj::object{{"error", "the panel only looks: " + tool + " isn't a read tool"}}),
                    json::dump(bj::object{{"panel", tool}, {"why", "not a read tool"}}));
    }
    // A focused person sees only their tenants here too (030 § 3).
    auto policy = operator_policy(me, true);
    if (policy.prepare) {
      if (const auto* t = mcp_->tool(tool)) policy.prepare(*t, args);
    }
    if (policy.check) {
      if (auto refused = policy.check(tool, args)) {
        return finish(json_reply(403, bj::object{{"error", *refused}}),
                      json::dump(bj::object{{"panel", tool}, {"why", "outside the operator's scope"}}));
      }
    }
    std::optional<ToolResult> out;
    {
      OperatorContext as(*this, me, request_id);
      out = mcp_->call(tool, args);
      if (out && !out->is_error && policy.filter) {
        if (auto refused = policy.filter(tool, out->value)) out = ToolResult::fail(*refused);
      }
    }
    bj::object extra{{"panel", tool}, {"operator", "panel:" + who->user.email}};
    {
      bj::object targs;
      for (const char* k : {"tenant", "path", "box", "name", "log"}) {
        if (auto* v = args.if_contains(k); v && v->is_string()) targs[k] = *v;
      }
      audit("operator.tool", me, tool, !out ? "refused" : out->is_error ? "failed" : "ok",
            bj::object{{"ip", client_address(req)}, {"request_id", request_id}, {"via", "panel"}, {"args", std::move(targs)}});
    }
    if (!out) return finish(json_reply(404, bj::object{{"error", "no such tool"}}), json::dump(extra));
    if (out->is_error) {
      extra["tool_error"] = true;
      return finish(json_reply(400, bj::object{{"error", out->value}}), json::dump(extra));
    }
    return finish(json_reply(200, bj::object{{"result", out->value}}), json::dump(extra));
  }
  finish(json_reply(404, bj::object{{"error", "not found"}}), R"({"why":"no such panel path"})");
}

void Host::record_bundle(const wal::Bundle& bundle) {
  if (!changes_) return;
  try {
    std::string actor = current_operator().empty() ? "host" : current_operator();
    changes_->record(bundle, actor, current_request_id(), base::now_unix_ms());
  } catch (const std::exception& e) {
    std::cerr << "goober-host: change log: " << e.what() << "\n";
  }
}

void Host::run_traffic() {
  std::unique_lock lock(timer_thread_mu_);
  while (!timers_stopping_) {
    lock.unlock();
    try {
      traffic_->catch_up(base::now_unix_ms());
      loop_beat("traffic");
    } catch (const std::exception& e) {
      std::cerr << "goober-host: traffic rollups: " << e.what() << "\n";
      loop_beat("traffic", e.what());
    }
    lock.lock();
    timer_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return timers_stopping_; });
  }
}

void Host::register_library_tools() {
  using mcp::object_schema;
  using mcp::string_property;
  mcp_->add_tool({"library_list",
                  "The host libraries a manifest can import (\"libraries\": {\"time\": \"time@1\"}) and call with goober_lib: "
                  "each name, major version, what it's for, and its functions (design/machine/831.Libraries.md).",
                  object_schema({}, {}), [](const bj::object&) {
                    bj::array out;
                    for (const auto& l : libraries::all()) {
                      bj::array fns;
                      for (const auto& f : l.functions) fns.push_back(bj::value(f.name));
                      out.push_back(bj::object{{"library", l.name + "@" + std::to_string(l.major)},
                                               {"description", l.description},
                                               {"functions", std::move(fns)}});
                    }
                    return ToolResult::ok(bj::object{{"libraries", std::move(out)}, {"tzdata", calendar::tzdata_version()}});
                  }});
  mcp_->add_tool({"library_get",
                  "One library's contract: every function with its input and output schema and examples.",
                  object_schema({{"library", string_property("Like time@1 or ics@1.")}}, {"library"}),
                  [](const bj::object& args) {
                    auto ref = libraries::parse_ref(arg_text(args, "library"));
                    const auto* lib = ref ? libraries::find(ref->first, ref->second) : nullptr;
                    if (!lib) return ToolResult::fail("no such library; library_list shows them");
                    return ToolResult::ok(libraries::describe(*lib));
                  }});
}

void Host::register_panel_tools() {
  using mcp::boolean_property;
  using mcp::enum_property;
  using mcp::integer_property;
  using mcp::object_schema;
  using mcp::string_property;

  auto window_prop = enum_property("The window: 1h, 12h (the default), 7d, or 30d.", {"1h", "12h", "7d", "30d"});
  auto window_of = [](const bj::object& args) {
    std::string w = arg_text(args, "window");
    return operations::Traffic::valid_window(w) ? w : std::string("12h");
  };

  mcp_->add_tool(
      {"traffic",
       "Requests over a window from the access log's rollups: a series stacked by status class with p50 and p99 "
       "latency, and every route's requests, errors, latency, fuel, SQL calls, and outbox calls. Pages and host work "
       "(identity, oauth, attachment, mcp, operations) are routes with no box.",
       object_schema({{"window", window_prop},
                      {"tenant", string_property("Only this tenant.")},
                      {"box", string_property("Only this box.")}},
                     {}),
       [this, window_of](const bj::object& args) {
         if (!traffic_) return ToolResult::fail("traffic rollups are off: see host_health's startup notes");
         int64_t now = base::now_unix_ms();
         traffic_->catch_up(now);
         std::string w = window_of(args), tenant = arg_text(args, "tenant"), box = arg_text(args, "box");
         return ToolResult::ok(bj::object{{"series", traffic_->series(w, tenant, box, now)},
                                          {"routes", traffic_->routes(w, tenant, box, now)}});
       }});

  mcp_->add_tool({"hot_boxes", "Boxes by heat (CPU time) over a window, with requests, server errors, fuel, and SQL calls.",
                  object_schema({{"window", window_prop}}, {}), [this, window_of](const bj::object& args) {
                    if (!traffic_) return ToolResult::fail("traffic rollups are off: see host_health's startup notes");
                    int64_t now = base::now_unix_ms();
                    traffic_->catch_up(now);
                    auto hot = traffic_->hot_boxes(window_of(args), now);
                    // Active boxes with no traffic are cold, not missing.
                    std::set<std::string> seen;
                    for (const auto& h : hot) seen.insert(std::string(h.as_object().at("box").as_string()));
                    for (const auto& name : boxes_->box_names()) {
                      if (!seen.count(name)) {
                        hot.push_back(bj::object{{"box", name}, {"n", 0}, {"n5", 0}, {"us", 0}, {"fuel", 0},
                                                 {"sql", 0}, {"share", 0.0}});
                      }
                    }
                    return ToolResult::ok(bj::object{{"window", window_of(args)}, {"boxes", std::move(hot)}});
                  }});

  mcp_->add_tool(
      {"box_graph",
       "The data flow over a window as nodes and edges. Without box: tenants, then their pages and boxes, then the "
       "databases, services, and boxes those call. With box: that box's routes, the box, and what it reaches. Edge n "
       "is requests or calls; us is CPU time (heat).",
       object_schema({{"window", window_prop}, {"box", string_property("One box's graph.")}}, {}),
       [this, window_of](const bj::object& args) {
         if (!traffic_) return ToolResult::fail("traffic rollups are off: see host_health's startup notes");
         int64_t now = base::now_unix_ms();
         traffic_->catch_up(now);
         std::string w = window_of(args), only = arg_text(args, "box");
         auto active = boxes_->active();
         if (!only.empty() && !active->boxes.count(only)) return ToolResult::fail("no active box named " + only);
         bj::array nodes, edges;
         std::set<std::string> have;
         auto node = [&](const std::string& id, const std::string& kind, const std::string& label, int column,
                         bj::object more = {}) {
           if (!have.insert(id).second) return;
           more["id"] = id;
           more["kind"] = kind;
           more["label"] = label;
           more["column"] = column;
           nodes.push_back(std::move(more));
         };
         auto edge = [&](const std::string& from, const std::string& to, int64_t n, int64_t errors, int64_t us,
                         bj::object more = {}) {
           more["from"] = from;
           more["to"] = to;
           more["n"] = n;
           more["errors"] = errors;
           more["us"] = us;
           edges.push_back(std::move(more));
         };
         auto routes = traffic_->routes(w, "", only, now);
         auto calls = traffic_->edges(w, only, now);
         // Box totals, for heat and for SQL edges.
         std::map<std::string, std::array<int64_t, 4>> box_sums;  // n, n5, us, sql
         for (const auto& r : routes) {
           const auto& o = r.as_object();
           std::string b(o.at("box").as_string());
           if (b.empty()) continue;
           auto& s = box_sums[b];
           s[0] += o.at("n").as_int64(), s[1] += o.at("n5").as_int64(), s[2] += o.at("us").as_int64();
           s[3] += o.at("sql").as_int64();
         }
         auto box_node = [&](const std::string& b, int column) {
           auto s = box_sums[b];
           auto it = active->boxes.find(b);
           node("box:" + b, "box", b, column,
                bj::object{{"n", s[0]}, {"n5", s[1]}, {"us", s[2]},
                           {"version", it == active->boxes.end() ? std::string() : it->second->version}});
         };
         // Each database's tables, one column further right: an edge per table, carrying every
         // box's grant on it (operations, columns) and whether the grant has been used since the
         // host started (grant_usage). Shared databases show each box's grant on one edge.
         std::map<std::pair<std::string, std::string>, bj::array> table_grants;
         std::map<std::pair<std::string, std::string>, int64_t> table_calls;
         auto columns_of = [](bool all, const std::set<std::string>& some) -> bj::value {
           if (all) return true;
           bj::array a;
           for (const auto& c : some) a.emplace_back(c);
           return a;
         };
         // What one box reaches: databases (SQL calls, split evenly when it has several), their
         // tables, services, and boxes it calls.
         auto outputs = [&](const std::string& b, int column) {
           auto it = active->boxes.find(b);
           if (it == active->boxes.end()) return;
           const auto& m = it->second->manifest;
           std::set<std::string> dbs;
           for (const auto& d : m.data) dbs.insert(d.database.empty() ? d.name : d.database);
           int64_t sql = box_sums[b][3];
           for (const auto& d : dbs) {
             node("db:" + d, "database", d, column);
             edge("box:" + b, "db:" + d, dbs.size() ? sql / int64_t(dbs.size()) : 0, 0, 0,
                  bj::object{{"approx", dbs.size() > 1}, {"what", "sql calls"}});
           }
           auto seen = grant_usage_ ? grant_usage_->of(b, it->second->version) : GrantUsage::Seen2{};
           for (const auto& st : m.data) {
             if (!st.runnable()) continue;
             std::string d = st.database.empty() ? st.name : st.database;
             for (const auto& [t, g] : st.tables) {
               GrantUsage::Seen s;
               if (auto ss = seen.find(st.name); ss != seen.end()) {
                 if (auto tt = ss->second.find(t); tt != ss->second.end()) s = tt->second;
               }
               std::string ops, used_ops;
               auto op = [&](bool granted, bool used, char c) {
                 if (granted) ops += c;
                 if (granted && used) used_ops += c;
               };
               op(g.read_all || !g.read.empty(), !s.use.read.empty(), 'r');
               op(g.insert, s.use.insert, 'i');
               op(g.update_all || !g.update.empty(), !s.use.update.empty(), 'u');
               op(g.remove, s.use.remove, 'd');
               node("table:" + d + "." + t, "table", t, column + 1, bj::object{{"database", d}});
               table_grants[{d, t}].push_back(bj::object{{"box", b},
                                                         {"strategy", st.name},
                                                         {"ops", ops},
                                                         {"used_ops", used_ops},
                                                         {"read", columns_of(g.read_all, g.read)},
                                                         {"update", columns_of(g.update_all, g.update)},
                                                         {"calls", s.calls}});
               table_calls[{d, t}] += s.calls;
             }
           }
           std::map<std::string, bj::object> seen_calls;
           for (const auto& c : calls) {
             const auto& o = c.as_object();
             if (o.at("from_box").as_string() != b) continue;
             std::string kind(o.at("to_kind").as_string()), name(o.at("to_name").as_string());
             std::string id = (kind == "box" ? "box:" : "svc:") + name;
             if (kind == "box") {
               box_node(name, column);
             } else {
               node(id, "service", name, column);
             }
             auto& e = seen_calls[id];
             for (const char* k : {"n", "errors", "us", "bytes_out", "bytes_in"}) {
               e[k] = (e.contains(k) ? e[k].as_int64() : 0) + o.at(k).as_int64();
             }
           }
           for (const auto& svc : m.external) {
             std::string id = svc.box.empty() ? "svc:" + svc.name : "box:" + svc.box;
             if (svc.box.empty()) {
               node(id, "service", svc.name, column,
                    bj::object{{"mail", svc.mail}, {"web", svc.web}, {"base", svc.base}});
             } else {
               box_node(svc.box, column);
             }
             seen_calls.try_emplace(id, bj::object{{"n", 0}, {"errors", 0}, {"us", 0}, {"bytes_out", 0}, {"bytes_in", 0}});
           }
           for (auto& [id, e] : seen_calls) {
             edge("box:" + b, id, e["n"].as_int64(), e["errors"].as_int64(), e["us"].as_int64(),
                  bj::object{{"bytes_out", e["bytes_out"]}, {"bytes_in", e["bytes_in"]}, {"what", "calls"}});
           }
         };

         if (only.empty()) {
           auto cfg = config_.get();
           std::map<std::string, bj::array> domains_of;
           for (const auto& [d, e] : cfg->domains) domains_of[e.tenant].push_back(bj::value(d));
           for (auto& [t, ds] : domains_of) node("tenant:" + t, "tenant", t, 0, bj::object{{"domains", ds}});
           // Tenant -> pages, host work, and boxes, from the rollups.
           std::map<std::pair<std::string, std::string>, std::array<int64_t, 3>> into;
           for (const auto& r : routes) {
             const auto& o = r.as_object();
             std::string t(o.at("tenant").as_string()), b(o.at("box").as_string()), route(o.at("route").as_string());
             if (t.empty() || !domains_of.count(t)) continue;
             std::string to;
             if (!b.empty()) {
               to = "box:" + b;
             } else if (route == "page") {
               to = "pages:" + t;
             } else if (route == "identity" || route == "oauth" || route == "attachment" || route == "mcp" ||
                        route == "unsubscribe" || route == "mail_in") {
               to = "host:" + t;
             } else {
               continue;
             }
             auto& s = into[{t, to}];
             s[0] += o.at("n").as_int64(), s[1] += o.at("n5").as_int64(), s[2] += o.at("us").as_int64();
           }
           for (const auto& [name, ab] : active->boxes) {
             box_node(name, 1);
             // A box's tenants, even without traffic yet.
             for (const auto& [t, ds] : domains_of) {
               bool serves = false;
               for (const auto& d : ds) serves |= ab->manifest.applies_to(std::string(d.as_string()));
               if (serves) into.try_emplace({t, "box:" + name}, std::array<int64_t, 3>{0, 0, 0});
             }
           }
           for (const auto& [k, s] : into) {
             const auto& [t, to] = k;
             if (to.starts_with("pages:")) node(to, "pages", "pages", 1);
             if (to.starts_with("host:")) node(to, "host", "sign-in, OAuth, files, MCP", 1);
             edge("tenant:" + t, to, s[0], s[1], s[2], bj::object{{"what", "requests"}});
           }
           for (const auto& [name, ab] : active->boxes) outputs(name, 2);
         } else {
           const auto& m = active->boxes.at(only)->manifest;
           std::map<std::string, const bj::object*> stats;
           for (const auto& r : routes) stats[std::string(r.as_object().at("route").as_string())] = &r.as_object();
           auto add_routes = [&](const std::vector<manifest::HttpRoute>& list, const std::string& protocol) {
             for (const auto& r : list) {
               std::string label = protocol == "http" ? base::to_lower(r.verb) + " " + r.pattern.source
                                   : protocol == "mail"  ? "mail " + r.mail_address
                                                         : protocol + " " + r.name;
               bj::object more{{"protocol", protocol}, {"route", r.name}, {"n", 0}, {"n5", 0}, {"us", 0},
                               {"p99_us", 0}, {"fuel", 0}};
               if (auto it = stats.find(r.name); it != stats.end()) {
                 for (const char* k : {"n", "n4", "n5", "us", "p50_us", "p99_us", "fuel", "sql", "outbox"}) {
                   more[k] = it->second->at(k);
                 }
               }
               int64_t n = more["n"].as_int64(), n5 = more["n5"].as_int64(), us = more["us"].as_int64();
               node("route:" + r.name, "route", label, 0, std::move(more));
               edge("route:" + r.name, "box:" + only, n, n5, us, bj::object{{"what", "requests"}});
             }
           };
           add_routes(m.routes, "http");
           add_routes(m.tools, "mcp");
           add_routes(m.timers, "timer");
           add_routes(m.mail, "mail");
           add_routes(m.internal, "internal");
           add_routes(m.identity, "identity");
           add_routes(m.results, "result");
           box_node(only, 1);
           // Boxes that call this one come in from the left.
           for (const auto& c : calls) {
             const auto& o = c.as_object();
             if (o.at("to_kind") == "box" && o.at("to_name").as_string() == only) {
               std::string from(o.at("from_box").as_string());
               box_node(from, 0);
               edge("box:" + from, "box:" + only, o.at("n").as_int64(), o.at("errors").as_int64(), o.at("us").as_int64(),
                    bj::object{{"what", "calls"}});
             }
           }
           outputs(only, 2);
         }
         for (auto& [key, grants] : table_grants) {
           const auto& [d, t] = key;
           std::set<char> ops;
           bool used = false;
           for (const auto& gr : grants) {
             for (char c : gr.at("ops").as_string()) ops.insert(c);
             used |= gr.at("calls").as_int64() > 0;
           }
           std::string label;
           for (char c : std::string("riud")) {
             if (ops.count(c)) label += c;
           }
           edge("db:" + d, "table:" + d + "." + t, table_calls[key], 0, 0,
                bj::object{{"what", "calls since start"}, {"ops", label}, {"unused", !used}, {"grants", std::move(grants)}});
         }
         return ToolResult::ok(bj::object{{"window", w}, {"box", only}, {"nodes", std::move(nodes)}, {"edges", std::move(edges)}});
       }});

  mcp_->add_tool(
      {"changes",
       "The change log, newest first: every deploy, version upload, content, partial, template, schema, and attachment "
       "write, with who made it (the operator, or host) and the request that caused it. Texts are in change_get.",
       object_schema({{"kind", enum_property("Only this kind.", {"deploy", "upload", "content", "partial", "template",
                                                                 "schema", "attachment", "other"})},
                      {"tenant", string_property("Only this tenant (_host for host-wide content).")},
                      {"before_id", integer_property("Rows older than this id, for the next page.")},
                      {"limit", integer_property("At most this many: default 50, at most 500.")}},
                     {}),
       [this](const bj::object& args) {
         if (!changes_) return ToolResult::fail("the change log is off: see host_health's startup notes");
         int64_t limit = std::clamp<int64_t>(arg_int(args, "limit", 50), 1, 500);
         return ToolResult::ok(bj::object{
             {"changes", changes_->list(arg_text(args, "kind"), arg_text(args, "tenant"), arg_int(args, "before_id", 0), limit)}});
       }});

  mcp_->add_tool({"change_get",
                  "One change with its text before and after (content and schemas of at most 64 KiB, for 90 days), or "
                  "for a deploy, both versions' manifests.",
                  object_schema({{"id", integer_property("The change's id, from changes.")}}, {"id"}),
                  [this](const bj::object& args) {
                    if (!changes_) return ToolResult::fail("the change log is off: see host_health's startup notes");
                    auto row = changes_->get(arg_int(args, "id", 0));
                    if (!row) return ToolResult::fail("no such change");
                    if (row->at("kind") == "deploy" || row->at("kind") == "upload") {
                      std::string box(row->at("target").as_string());
                      const auto& d = row->at("detail").as_object();
                      for (auto [field, key] : {std::pair{"previous", "manifest_before"}, std::pair{"version", "manifest_after"}}) {
                        auto* v = d.if_contains(field);
                        if (!v || !v->is_string()) continue;
                        if (auto text = boxes_->manifest_text(box, std::string(v->as_string()))) (*row)[key] = *text;
                      }
                    }
                    return ToolResult::ok(std::move(*row));
                  }});

  mcp_->add_tool({"operator_actions",
                  "Every operator tool call, newest first: who, which tool, its tenant/path/box/name arguments, whether "
                  "it failed, and the ids of the changes it made. changed_only keeps the calls that changed something.",
                  object_schema({{"changed_only", boolean_property("Only calls that changed something.")},
                                 {"before_id", integer_property("Rows older than this id, for the next page.")},
                                 {"limit", integer_property("At most this many: default 50, at most 500.")}},
                                {}),
                  [this](const bj::object& args) {
                    if (!changes_) return ToolResult::fail("the change log is off: see host_health's startup notes");
                    auto* c = args.if_contains("changed_only");
                    bool changed = c && c->is_bool() && c->as_bool();
                    int64_t limit = std::clamp<int64_t>(arg_int(args, "limit", 50), 1, 500);
                    return ToolResult::ok(
                        bj::object{{"actions", changes_->actions(changed, arg_int(args, "before_id", 0), limit)}});
                  }});
}

}  // namespace goober::host
