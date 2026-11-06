// The operator panel (design/machine/028.Panel.md): its own origin (operations.domain, or a
// listener of its own), host-realm sign-in, static assets compiled into the binary, and
// POST /api/$tool for the read tools. Also the tools only the panel needed until now:
// traffic, hot_boxes, box_graph, changes, change_get, and operator_actions.

#include "build_id.h"

#include <iostream>
#include <map>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/calendar/zones.h"
#include "goober/libraries/libraries.h"
#include "goober/operations/changes.h"
#include "goober/operations/traffic.h"
#include "goober/sqlite/sqlite.h"
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
    "log_status",      "log_velocity",      "uri_stats",         "uri_tree",         "uri_flow",        "agent_audit",
    "source_checkout",
    "whats_new",
    "version_diff", "version_health",
    "perf_report", "perf_statements",
    "cache_status",
    "error_report", "box_failures", "service_report",
    "spec_list",       "spec_get",          "ids_status",          "approval_status",  "bus_status",      "box_render",
    "source_tree",     "source_box_history", "grant_usage",     "job_list",        "job_status",
    "mail_domain_check", "mail_route_check",
    "deletions",
    "schema_check",
    "flag_list",
    "control_list", "control_report", "mcp_queue_report",
    "db_snapshots", "db_unbound",
    "route_try",
    "changeset_show", "changeset_list", "changeset_preview",
    "heat_study_get", "heat_study_list",
    "skill_list", "skill_search", "skill_get",
    "cleanup_report", "split_report", "split_list", "beta_status", "beta_mail_list", "beta_mail_get", "promotion_preview"};

const std::set<std::string, std::less<>> kPanelRoles = {"host:owner", "host:operator", "host:observer", "host:beta"};

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
  // A string of digits too: agents told "ids are strings in JSON" send them either way.
  if (v && v->is_string()) {
    std::string_view t = v->as_string();
    if (t.empty() || t.size() > 18 || t.find_first_not_of("0123456789") != std::string_view::npos) return fallback;
    int64_t n = 0;
    for (char c : t) n = n * 10 + (c - '0');
    return n;
  }
  return fallback;
}

}  // namespace

bool Host::is_read_tool(std::string_view tool) { return kReadTools.count(tool) > 0; }

bool Host::is_read_call(std::string_view tool, const bj::object& args) {
  auto flag = [&](const char* k) {
    const auto* v = args.if_contains(k);
    return v && v->is_bool() && v->as_bool();
  };
  if (tool == "mail_unsubscribed") return !args.contains("set");
  if (tool == "version_upload" || tool == "schema_apply") return flag("dry_run");
  // A drop's dry run only checks (051 § 4); the drop itself always waits for a person.
  if (tool == "box_drop" || tool == "db_drop") return flag("dry_run");
  if (tool == "beta_seed") return flag("dry_run");
  // A content dry run stores nothing; staging builds only the operator's own draft, which no
  // one sees until a content_deploy (which waits) writes it.
  if (tool == "content_deploy") return flag("dry_run");
  if (tool == "content_stage") return true;
  // An export hands the agent no rows: they're sealed for a person to fetch (008 § 7.1). Not a
  // read tool, though, so observers and the look-only panel can't make one.
  if (tool == "db_export") return true;
  // A heat study only counts what is served (048 § 2): it never changes how anything is
  // served, so it never waits for approval. Not a read tool: observers and the panel can't
  // start or stop one.
  if (tool == "heat_study_start" || tool == "heat_study_stop") return true;
  // A change set's draft is the same: nothing is live until changeset_apply (which waits).
  if (tool == "changeset_create" || tool == "changeset_add" || tool == "changeset_discard") return true;
  // A try only looks when it is an HTTP GET or HEAD (read-only on live data); a write's, a
  // tool's, or a mail route's try runs box code on copies, which is more than looking, so
  // observers and the panel (look-only for everyone) can't (approvals never hold a try,
  // approvals.cpp: it changes nothing real).
  if (tool == "route_try") {
    if (args.contains("tool") || args.contains("mail")) return false;
    const auto* m = args.if_contains("method");
    std::string method = m && m->is_string() ? base::to_lower(m->as_string()) : "get";
    return method == "get" || method == "head";
  }
  return is_read_tool(tool);
}

std::string& Host::current_request_id() {
  thread_local std::string id;
  return id;
}

std::vector<std::string> Host::operator_roles(const std::string& name) const {
  // Through beta-ops (053 § 6) an operator keeps its roles; its scope is beta's.
  std::string operator_name(beta_unwrapped(name));
  // The config's token and minted tokens are the owner's own (design/machine/023 § 3).
  if (operator_name == "config" || operator_name.starts_with("token:")) return {"host:owner"};
  if (operator_name.starts_with("panel:")) {
    auto cfg = config_.get();
    if (auto it = cfg->identity.operators.find(operator_name.substr(6)); it != cfg->identity.operators.end()) return it->second;
    return {};
  }
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
    // host:beta changes beta alone: its scope holds it there (053 § 6).
    if (r == "host:owner" || r == "host:operator" || r == "host:beta") return true;
  }
  return false;
}

bool Host::is_panel_request(const Request& req, bool panel_listener) const {
  if (panel_listener) return true;
  std::string domain = config_.get()->panel_domain();
  std::string h = base::to_lower(req.header("host"));
  if (auto colon = h.rfind(':'); colon != std::string::npos && h.find(']') == std::string::npos) h = h.substr(0, colon);
  if (!h.empty() && h.back() == '.') h.pop_back();
  return (!domain.empty() && h == domain) || is_beta_ops_request(req);
}

bool Host::is_beta_ops_request(const Request& req) const {
  std::string domain = config_.get()->beta_panel_domain();
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
      bool beta_ops = is_beta_ops_request(req);
      auto scope = operator_scope((beta_ops ? "beta:panel:" : "panel:") + who->user.email);
      bool owner = std::find(who->roles.begin(), who->roles.end(), "host:owner") != who->roles.end();
      // The beta sites (053), for the Beta view: beta-ops shows them alone.
      bj::array beta_sites;
      for (const auto& [d, e] : cfg->domains) {
        if (!e.beta_of.empty()) beta_sites.push_back(bj::object{{"domain", d}, {"tenant", e.tenant}, {"beta_of", e.beta_of}});
      }
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
                                               {"beta_ops", beta_ops},
                                               {"beta_sites", std::move(beta_sites)},
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
    // beta-ops (053 § 6): the same panel, its every call in beta's scope; it looks, and it injects mail.
    bool beta_ops = is_beta_ops_request(req);
    std::string me = (beta_ops ? "beta:panel:" : "panel:") + who->user.email;
    if (beta_ops && tool.starts_with("write/") && tool != "write/beta_mail_send" && tool != "write/beta_mail_clear") {
      return finish(json_reply(403, bj::object{{"error", "beta-ops looks and injects mail; production's changes are made in the panel"}}),
                    json::dump(bj::object{{"panel", tool}, {"why", "beta-ops"}}));
    }
    // The panel's own writes: the only changes it makes, each a human's (030 § 2, 029 § 6).
    // The panel's own answers are {"result": ...}, as a tool's are, which is what its script reads.
    auto own = [](int status, bj::object body) {
      return status == 200 ? json_reply(200, bj::object{{"result", std::move(body)}}) : json_reply(status, body);
    };
    if (tool == "write/beta_mail_send") {
      if (!may_change(who->roles)) return finish(json_reply(403, bj::object{{"error", "an observer can't send mail into beta"}}), "");
      std::string err;
      bj::object out;
      {
        OperatorContext as(*this, me, request_id);
        out = beta_mail_inject(arg_text(args, "to"), arg_text(args, "from"), arg_text(args, "subject"), arg_text(args, "text"),
                               arg_text(args, "in_reply_to"), &err);
      }
      if (!err.empty()) return finish(json_reply(400, bj::object{{"error", err}}), json::dump(bj::object{{"panel", tool}}));
      return finish(own(200, std::move(out)), json::dump(bj::object{{"panel", tool}, {"operator", me}}));
    }
    // Emptying a beta mailbox (053 § 4): beta's own, so beta-ops may too.
    if (tool == "write/beta_mail_clear") {
      bool beta_role = std::find(who->roles.begin(), who->roles.end(), "host:beta") != who->roles.end();
      if (!may_change(who->roles) && !beta_role) return finish(json_reply(403, bj::object{{"error", "an observer can't empty a mailbox"}}), "");
      std::string tenant = arg_text(args, "tenant");
      auto scope = operator_scope(me);
      if (!scope.all && !scope.tenants.count(tenant)) {
        return finish(json_reply(403, bj::object{{"error", "you're focused on other tenants, not " + tenant}}), "");
      }
      std::string err;
      std::optional<bj::object> out;
      {
        OperatorContext as(*this, me, request_id);
        out = beta_mail_clear(tenant, args, &err);
      }
      if (!out) return finish(json_reply(400, bj::object{{"error", err}}), json::dump(bj::object{{"panel", tool}}));
      return finish(own(200, std::move(*out)), json::dump(bj::object{{"panel", tool}, {"operator", me}}));
    }
    // The operators' mailbox (055): admin@, postmaster@, abuse@, read and answered by people with
    // host:owner or host:operator, within their focus; never on beta-ops.
    if (tool.starts_with("operator_mail") || tool.starts_with("write/operator_mail")) {
      if (beta_ops) return finish(json_reply(403, bj::object{{"error", "the operators' mail is read in the panel, not beta-ops"}}), "");
      if (!may_change(who->roles)) {
        return finish(json_reply(403, bj::object{{"error", "the operators' mail is for host:owner and host:operator"}}), "");
      }
      auto scope = operator_scope(me);
      std::string id = arg_text(args, "id");
      bj::object log{{"panel", tool}, {"operator", me}};
      if (tool == "operator_mail") {
        std::string folder = arg_text(args, "folder");
        if (folder != "sent" && folder != "all") folder = "inbox";
        return finish(own(200, bj::object{{"folder", folder},
                                          {"messages", operator_mail_list(scope, folder, std::clamp<int64_t>(arg_int(args, "limit", 100), 1, 500))},
                                          {"addresses", operator_mail_addresses(scope)}}),
                      json::dump(log));
      }
      if (tool == "operator_mail_get") {
        auto m = operator_mail_get(scope, id);
        if (!m) return finish(json_reply(404, bj::object{{"error", "no message " + id + " in the mailbox you may read"}}), json::dump(log));
        return finish(own(200, std::move(*m)), json::dump(log));
      }
      if (tool == "operator_mail_attachment") {
        auto a = operator_mail_attachment(scope, id, arg_int(args, "index", -1));
        if (!a) return finish(json_reply(404, bj::object{{"error", "no such attachment"}}), json::dump(log));
        return finish(own(200, std::move(*a)), json::dump(log));
      }
      if (tool == "write/operator_mail_send") {
        std::string err;
        auto out = operator_mail_send(scope, me, args, &err);
        if (!out) return finish(json_reply(400, bj::object{{"error", err}}), json::dump(log));
        return finish(own(200, std::move(*out)), json::dump(log));
      }
      if (tool == "write/operator_mail_delete") {
        if (!operator_mail_delete(scope, id)) return finish(json_reply(404, bj::object{{"error", "no message " + id + " in the mailbox you may read"}}), json::dump(log));
        audit("operators.mail_delete", me, id, "ok", bj::object{{"ip", client_address(req)}, {"request_id", request_id}});
        return finish(own(200, bj::object{{"id", id}, {"deleted", true}}), json::dump(log));
      }
      return finish(json_reply(404, bj::object{{"error", "not found"}}), json::dump(log));
    }
    // Promotion (053 § 11): the owner carries beta to production as one change set, applied at
    // once, and rolls one back. Typing the tenant's name confirms.
    if (tool == "write/promote" || tool == "write/promotion_rollback") {
      if (std::find(who->roles.begin(), who->roles.end(), "host:owner") == who->roles.end()) {
        return finish(json_reply(403, bj::object{{"error", "only host:owner promotes beta to production"}}), "");
      }
      bj::object out;
      std::string err;
      {
        OperatorContext as(*this, me, request_id);
        if (tool == "write/promote") {
          std::string tenant = arg_text(args, "tenant");
          if (arg_text(args, "confirm") != tenant || tenant.empty()) {
            return finish(json_reply(400, bj::object{{"error", "type the tenant's name to promote its beta"}}), "");
          }
          auto started = promotion_start(tenant, args, &err);
          if (!started) return finish(json_reply(400, bj::object{{"error", err}}), "");
          bj::object apply{{"id", started->at("changeset")}};
          if (started->at("draft_version").is_int64()) apply["version"] = started->at("draft_version");
          auto applied = mcp_->call("changeset_apply", apply);
          out = bj::object{{"changeset", started->at("changeset")},
                           {"applied", applied && !applied->is_error},
                           {"result", applied ? applied->value : bj::value(nullptr)}};
          if (!applied || applied->is_error) err = "the change set didn't apply (nothing stayed changed): " + (applied ? json::dump(applied->value) : std::string("no tool"));
        } else {
          auto rolled = mcp_->call("changeset_rollback", bj::object{{"id", arg_text(args, "changeset")}});
          out = bj::object{{"changeset", arg_text(args, "changeset")}, {"result", rolled ? rolled->value : bj::value(nullptr)}};
          if (!rolled || rolled->is_error) err = rolled ? json::dump(rolled->value) : "no tool";
        }
      }
      audit(tool == "write/promote" ? "panel.promote" : "panel.promotion_rollback", me, arg_text(args, "tenant"),
            err.empty() ? "ok" : "failed", bj::object{{"ip", client_address(req)}, {"request_id", request_id}});
      if (!err.empty()) return finish(json_reply(400, bj::object{{"error", err}, {"detail", out}}), "");
      return finish(own(200, std::move(out)), json::dump(bj::object{{"panel", tool}, {"operator", me}}));
    }
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
    // Dropping a database, a table, or a snapshot, retiring a schema (008 § 11), or approving a
    // schema that drops or rewrites data (041 § 6): a person's act.
    if (tool == "write/drop") {
      auto [status, body] = panel_drop(*who, args, request_id);
      audit(arg_text(args, "kind") == "schema_approve" ? "panel.schema_approve" : "panel.drop", me, arg_text(args, "kind") + ":" + arg_text(args, "tenant") + "/" + arg_text(args, "confirm"),
            status == 200 ? "ok" : "refused", bj::object{{"ip", client_address(req)}, {"request_id", request_id}});
      return finish(own(status, body), json::dump(bj::object{{"panel", tool}, {"operator", me}}));
    }
    // Setting, changing, observing, or removing a per-domain control (047 § 9): a person's act at
    // any time, since a control may be bum (the human, 2026-11-03). Audited as
    // panel.control_set or panel.control_remove, in the change log, never held for approval.
    if (tool == "write/control_set" || tool == "write/control_remove") {
      auto [status, body] = panel_control(*who, tool == "write/control_set" ? "set" : "remove", args, request_id);
      if (status != 200) {
        audit(tool == "write/control_set" ? "panel.control_set" : "panel.control_remove", me,
              "control:" + arg_text(args, "domain"), "refused",
              bj::object{{"ip", client_address(req)}, {"request_id", request_id}, {"error", body.contains("error") ? body.at("error") : bj::value(nullptr)}});
      }
      return finish(own(status, body), json::dump(bj::object{{"panel", tool}, {"operator", me}}));
    }
    if (!is_read_tool(tool) || !is_read_call(tool, args)) {
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
      traffic_->set_budget(config_.get()->disk.traffic_bytes);  // limits.traffic-bytes, as reloaded
      control_rows_to_traffic(base::now_unix_ms());
      traffic_->catch_up(base::now_unix_ms());
      if (failures_) failures_->flush(base::now_unix_ms());
      refresh_log_velocity(base::now_unix_ms());  // what readiness reads, so it never computes it
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

  auto window_prop = enum_property("The window: 1h, 12h (the default), or 7d (five-minute buckets, kept seven days).", {"1h", "12h", "7d"});
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

  mcp_->add_tool(
      {"error_report",
       "What visitors and clients got that wasn't a success, from the access log's rollups (028 § 4.2): every 4xx and "
       "5xx over the window as a series against all requests, by status, and by place (a box's route, a page's path, a "
       "host area like identity or mcp, or \"(not found)\" for every path nothing answers), each split by who made it "
       "(error_by: host, when no box ran: no such page, a gate, a refused input, a limit; box, when a box answered it; "
       "box_failed, when a box ran and failed) and by whether the visitor followed a link from this site (same), "
       "another site (other), or none. Box-to-box calls and route_try are left out. box_failures says why boxes "
       "failed. A read tool.",
       object_schema({{"window", window_prop}, {"tenant", string_property("Only this tenant.")}}, {}),
       [this, window_of](const bj::object& args) {
         if (!traffic_) return ToolResult::fail("traffic rollups are off: see host_health's startup notes");
         int64_t now = base::now_unix_ms();
         traffic_->catch_up(now);
         return ToolResult::ok(traffic_->errors(window_of(args), arg_text(args, "tenant"), now));
       }});

  mcp_->add_tool(
      {"box_failures",
       "How boxes fail (028 § 4.3), from the host's record of every box call (requests, tools, timers, mail, jobs, "
       "internal calls): per box and route, calls and the failures by class, a series, and the latest messages "
       "(newest first). Tries and dry runs (route_try, job_dry_run) count in none of that: their failures are listed "
       "apart under tries, each with trial (route_try or job_dry_run), so a failed dry run says why. Classes, by "
       "group: call (the call failed: trap, timeout, out_of_fuel, failed, no_response, bad_response, bad_status, "
       "render_failed, outbox_failed, host_write_failed, commit_failed, host_error, job_failed: a job's batch the "
       "host couldn't take, like an answer without a cursor or a cursor that didn't move); sql (statements refused or failed, per statement, even when the call went on: "
       "sql_error, no_clock, refused_write); outbox (deliveries the box queued: delivery_retry, delivery_dead); "
       "internal (a call to another box failed, on the caller: internal_failed); answer (a mail route chose: "
       "mail_rejected, mail_deferred). failures counts the call group alone. box_logs has each call's lines by "
       "request_id. A read tool.",
       object_schema({{"window", window_prop},
                      {"tenant", string_property("Only this tenant.")},
                      {"box", string_property("Only this box.")},
                      {"latest", integer_property("How many latest messages, 1 to 200 (default 50).")}},
                     {}),
       [this, window_of](const bj::object& args) {
         if (!failures_) return ToolResult::fail("box failures are off: see host_health's startup notes");
         int64_t now = base::now_unix_ms();
         failures_->flush(now);
         size_t latest = size_t(std::clamp<int64_t>(arg_int(args, "latest", 50), 1, 200));
         return ToolResult::ok(
             failures_->report(window_of(args), arg_text(args, "tenant"), arg_text(args, "box"), now, latest));
       }});

  mcp_->add_tool(
      {"service_report",
       "What went out to services over a window (028 § 4.4), from the egress and mail logs: boxes' calls through "
       "goober_call and the outbox, the host's push sends, robot fetches (web), token refreshes, and every mail the "
       "mailer sent (service mailer, method ses, smtp, or dev; box boxes or host). Answers totals and a series "
       "(calls, failed, outbox deliveries, p50 and p99 latency), each service (kind: external, mail, web, push, or "
       "mailer; calls, failed, n4, n5, neterr for no answer, bytes, the boxes and tenants calling it, and the "
       "outbox's retries and dead letters from box_failures), each method per box, failure statuses, and the latest "
       "failures (newest first, with url, request_id, or outbox_id). Also: the outbox's waiting elements over the "
       "window and now (outbox), the mail limits (mail: daily-max, sent in the last 24 hours per tenant; bounces and "
       "complaints aren't reported back to the host), mail in (mail_in: hook calls, refused), and, without tenant, "
       "backups (backup_status's summary). service narrows to one. A read tool.",
       object_schema({{"window", window_prop},
                      {"tenant", string_property("Only this tenant.")},
                      {"service", string_property("Only this service, like mailer, web, push, or a box's external service.")},
                      {"latest", integer_property("How many latest failures, 1 to 200 (default 50).")}},
                     {}),
       [this, window_of](const bj::object& args) {
         if (!traffic_) return ToolResult::fail("traffic rollups are off: see host_health's startup notes");
         int64_t now = base::now_unix_ms();
         traffic_->catch_up(now);
         std::string w = window_of(args), tenant = arg_text(args, "tenant"), service = arg_text(args, "service");
         size_t latest = size_t(std::clamp<int64_t>(arg_int(args, "latest", 50), 1, 200));
         auto out = traffic_->services(w, tenant, service, now, latest);
         // What each service is: a box's external service (mail and web are special), or the host's.
         std::map<std::string, std::string> kinds{{"mailer", "mailer"}, {"push", "push"}, {"web", "web"}};
         auto active = boxes_->active();
         for (const auto& [name, ab] : active->boxes) {
           for (const auto& svc : ab->manifest.external) {
             if (!svc.box.empty()) continue;  // a box adapter: an internal call, not a service
             kinds[svc.name] = svc.mail ? "mail" : svc.web ? "web" : "external";
           }
         }
         // The outbox's retries and dead letters per service, from box_failures (028 § 4.3).
         std::map<std::string, std::pair<int64_t, int64_t>> outbox_failed;
         if (failures_) {
           failures_->flush(now);
           auto r = failures_->report(w, tenant, "", now, 1);
           for (const auto& v : r.at("routes").as_array()) {
             const auto& o = v.as_object();
             if (o.at("kind") != "outbox") continue;
             std::string route(o.at("route").as_string());
             std::string svc = route.substr(0, route.find('.'));
             const auto& cls = o.at("classes").as_object();
             auto count = [&](const char* k) { return cls.contains(k) ? cls.at(k).as_int64() : int64_t(0); };
             outbox_failed[svc].first += count("delivery_retry");
             outbox_failed[svc].second += count("delivery_dead");
           }
         }
         std::set<std::string> listed;
         for (auto& v : out["services"].as_array()) {
           auto& o = v.as_object();
           std::string name(o.at("service").as_string());
           listed.insert(name);
           o["kind"] = kinds.count(name) ? kinds[name] : "external";
           o["retries"] = outbox_failed[name].first;
           o["dead"] = outbox_failed[name].second;
         }
         // Services the active boxes declare, even with no calls in the window.
         if (service.empty()) {
           for (const auto& [name, kind] : kinds) {
             if (listed.count(name) || kind == "mailer" || kind == "push" || (kind == "web" && name == "web")) continue;
             out["services"].as_array().push_back(bj::object{{"service", name}, {"kind", kind}, {"calls", 0}, {"failed", 0},
                                                             {"n4", 0}, {"n5", 0}, {"neterr", 0}, {"outbox", 0},
                                                             {"retries", outbox_failed[name].first},
                                                             {"dead", outbox_failed[name].second}});
           }
         }
         out["outbox"] = traffic_->gauge_series(w, "outbox_pending", tenant, now);
         // The mail limits: what each tenant may send a day, and what it sent in the last 24 hours.
         auto cfg = config_.get();
         bj::array sent;
         try {
           std::lock_guard lock(sent_mu_);
           sqlite::Stmt s(sent_ledger(), "SELECT tenant, count(*) FROM sent WHERE at_ms > ? AND (? = '' OR tenant = ?) "
                                         "GROUP BY tenant ORDER BY count(*) DESC");
           s.bind(1, now - 86400000).bind(2, tenant).bind(3, tenant);
           while (s.step()) sent.push_back(bj::object{{"tenant", s.text(0)}, {"sent_24h", s.int64(1)}});
         } catch (const std::exception& e) {
           std::cerr << "goober-host: service_report: the sent ledger: " << e.what() << "\n";
         }
         out["mail"] = bj::object{{"mailer", cfg->identity.mailer},
                                  {"daily_max", cfg->mail.daily_max},
                                  {"per_address_daily", cfg->mail.per_address_daily},
                                  {"tenants", std::move(sent)},
                                  {"bounces", "not reported: the host doesn't receive the mailer's bounce or complaint notices"}};
         int64_t in_n = 0, in_n4 = 0, in_n5 = 0;
         for (const auto& r : traffic_->routes(w, tenant, "", now)) {
           const auto& o = r.as_object();
           if (o.at("route") != "mail_in" || !o.at("box").as_string().empty()) continue;
           in_n += o.at("n").as_int64(), in_n4 += o.at("n4").as_int64(), in_n5 += o.at("n5").as_int64();
         }
         out["mail_in"] = bj::object{{"calls", in_n}, {"n4", in_n4}, {"n5", in_n5}};
         if (tenant.empty()) {
           auto b = backup_status(false);
           bj::object brief;
           for (const char* k : {"enabled", "healthy", "note", "last_nightly_at", "last_increment_at", "assets", "last_error",
                                 "last_error_at", "key_confirmed_at"}) {
             if (const auto* v = b.if_contains(k)) brief[k] = *v;
           }
           out["backups"] = std::move(brief);
         }
         return ToolResult::ok(std::move(out));
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
       "databases, services, and boxes those call, then each database's tables. With box: that box's routes (http, "
       "mcp, timer, mail, internal, identity, result, job), the box, and what it reaches. Edge n is requests or "
       "calls over the window, except database-to-table edges, which count calls since the host started and carry "
       "ops (the granted r, i, u, d), unused, and each box's grant; us is CPU time (heat). grant_usage has the "
       "detail per grant.",
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
         std::set<std::string> mail_services;         // service nodes that are mail
         std::map<std::string, int64_t> mail_handed;  // mail service node -> deliveries that went
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
           // The manifest's services first, so their nodes say what they are (mail, the web).
           for (const auto& svc : m.external) {
             std::string id = svc.box.empty() ? "svc:" + svc.name : "box:" + svc.box;
             if (svc.box.empty()) {
               node(id, "service", svc.name, column,
                    bj::object{{"mail", svc.mail}, {"web", svc.web}, {"base", svc.base}});
               if (svc.mail) mail_services.insert(id);
             } else {
               box_node(svc.box, column);
             }
             seen_calls.try_emplace(id, bj::object{{"n", 0}, {"errors", 0}, {"us", 0}, {"bytes_out", 0}, {"bytes_in", 0}});
           }
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
           for (auto& [id, e] : seen_calls) {
             edge("box:" + b, id, e["n"].as_int64(), e["errors"].as_int64(), e["us"].as_int64(),
                  bj::object{{"bytes_out", e["bytes_out"]}, {"bytes_in", e["bytes_in"]}, {"what", "calls"}});
             // Mail a box's mail service handed on, delivered to the mailer (§ 6).
             if (mail_services.count(id)) mail_handed[id] += e["n"].as_int64() - e["errors"].as_int64();
           }
         };
         // The mailer (ses, smtp, or dev) every mail goes out through, right of the services:
         // each mail service's delivered mail, and the host's own (sign-in codes, invites) from
         // the tenant's host routes. Each service node carries its calls, failures, and p99.
         auto services_and_mailer = [&](int column, bool host_graph) {
           auto svc = traffic_->services(w, "", "", now, 0);
           std::map<std::string, const bj::object*> by_name;
           for (const auto& s : svc.at("services").as_array()) by_name[std::string(s.at("service").as_string())] = &s.as_object();
           int64_t host_mail = 0, host_mail_failed = 0;
           std::map<std::string, std::pair<int64_t, int64_t>> host_mail_of;  // tenant -> sent, failed
           for (const auto& m : svc.at("methods").as_array()) {
             const auto& o = m.as_object();
             if (o.at("service") != "mailer" || o.at("box") != "host") continue;
             auto& hm = host_mail_of[std::string(o.at("tenant").as_string())];
             hm.first += o.at("calls").as_int64() - o.at("failed").as_int64();
             hm.second += o.at("failed").as_int64();
             host_mail += o.at("calls").as_int64();
             host_mail_failed += o.at("failed").as_int64();
           }
           bool mailer = !mail_services.empty() || (host_graph && host_mail > 0);
           if (mailer) {
             auto cfg = config_.get();
             bj::object more{{"mailer", true}, {"via", cfg->identity.mailer}};
             if (auto it = by_name.find("mailer"); it != by_name.end()) {
               for (const char* k : {"calls", "failed", "p99_us"}) more[k] = it->second->at(k);
             }
             node("svc:mailer", "service", "mailer (" + cfg->identity.mailer + ")", column, std::move(more));
             for (const auto& [id, n] : mail_handed) edge(id, "svc:mailer", n, 0, 0, bj::object{{"what", "mails handed on"}});
             if (host_graph) {
               for (const auto& [t, sf] : host_mail_of) {
                 std::string from = have.count("host:" + t) ? "host:" + t : "host:_host";
                 if (from == "host:_host") node(from, "host", "the host", column - 2);
                 edge(from, "svc:mailer", sf.first + sf.second, sf.second, 0, bj::object{{"what", "host mails"}});
               }
             }
           }
           // Each service's calls, failures, and p99 over the window, on its node.
           for (auto& v : nodes) {
             auto& o = v.as_object();
             if (o.at("kind") != "service" || o.contains("mailer")) continue;
             std::string name(o.at("label").as_string());
             if (auto it = by_name.find(name); it != by_name.end()) {
               for (const char* k : {"calls", "failed", "p99_us"}) o[k] = it->second->at(k);
             } else {
               o["calls"] = 0;
               o["failed"] = 0;
             }
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
           // The host's own calls out (push sends, token refreshes, the robot's fetches): from
           // the tenant's host routes, or from the host when no tenant is named.
           std::map<std::pair<std::string, std::string>, std::array<int64_t, 3>> host_calls;
           for (const auto& c : calls) {
             const auto& o = c.as_object();
             std::string from_box(o.at("from_box").as_string());
             if (o.at("to_kind") != "service" || (!from_box.empty() && from_box != "operator")) continue;
             std::string t(o.at("tenant").as_string());
             std::string from = domains_of.count(t) ? "host:" + t : "host:_host";
             auto& s = host_calls[{from, std::string(o.at("to_name").as_string())}];
             s[0] += o.at("n").as_int64(), s[1] += o.at("errors").as_int64(), s[2] += o.at("us").as_int64();
           }
           for (const auto& [k, s] : host_calls) {
             const auto& [from, name] = k;
             node(from, "host", from == "host:_host" ? "the host" : "sign-in, OAuth, files, MCP", 1);
             node("svc:" + name, "service", name, 2, bj::object{{"web", name == "web"}});
             edge(from, "svc:" + name, s[0], s[1], s[2], bj::object{{"what", "host calls"}});
           }
           services_and_mailer(3, true);
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
           add_routes(m.jobs, "job");
           add_routes(m.upgrades, "upgrade");
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
           services_and_mailer(3, false);
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
       "The change log, newest first: every deploy (activate or disable), version upload, content, partial, "
       "template, schema, flag, control, and attachment write, with who made it (the operator, or host) and the request that "
       "caused it. Each row's id is the change_id a write answered with. Texts are in change_get. Operator calls "
       "that change things outside the log (panel drops, snapshots, source commits) are in operator_actions.",
       object_schema({{"kind", enum_property("Only this kind.", {"deploy", "upload", "content", "partial", "template",
                                                                 "schema", "flag", "control", "skill", "attachment", "other"})},
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
                  "One change, by the id changes lists (a write's change_id): its text before and after (content, "
                  "templates, partials, schemas, and flags of at most 64 KiB, kept 90 days), or for a deploy or upload, "
                  "both versions' manifests.",
                  object_schema({{"id", integer_property("The change's id: a number, from changes or a write's change_id.")}}, {"id"}),
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
