// Operator scope (design/machine/030.OperatorScope.md): which tenants an operator acts on,
// kept per operator in storage/operations/scopes.json and changed only in the panel. Every
// operator tool has a rule; a focused operator's calls are checked against it, and list
// results are filtered to the scope.

#include <algorithm>
#include <iostream>
#include <map>
#include <set>

#include "builder.h"
#include "goober/base/files.h"
#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/host/operator_tokens.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

using Rule = Host::ScopeRule;

// Every operator tool's rule (§ 3). A tool missing here is refused for a focused operator, and
// a test fails when a registered tool has no rule.
const std::map<std::string, Rule, std::less<>>& rules() {
  static const std::map<std::string, Rule, std::less<>> kRules = [] {
    std::map<std::string, Rule, std::less<>> m;
    for (const char* t :
         {"agent_token_mint", "attachment_delete", "attachment_get", "attachment_list", "attachment_put",
          "attachment_upload_url", "changes", "client_list", "client_put", "client_remove", "content_delete",
          "content_get", "content_list", "content_put", "db_describe", "db_exec", "db_list", "db_query", "key_delete",
          "key_set", "mail_activity", "mail_template_get", "mail_template_list", "mail_template_preview", "mail_template_send_test",
          "mail_template_put", "mail_template_remove", "mail_unsubscribed", "outbox_discard", "outbox_retry",
          "page_get", "page_list", "page_put", "page_render", "partial_delete", "partial_get", "partial_list",
          "partial_put", "push_send", "pwa_delete", "pwa_get", "pwa_list", "pwa_put", "pwa_subscriptions",
          "realm_get", "role_grant", "role_revoke", "sessions_revoke", "template_get", "template_list",
          "template_put", "template_delete", "timer_run", "traffic", "invite_send", "invite_list", "invite_get", "invite_revoke",
          "mail_preferences", "mail_consent_record", "mail_lists", "content_deploy", "list_etags",
          "page_lint", "approval_record", "approval_verify", "approval_list", "uri_tree", "uri_flow", "bus_status", "box_render",
          "job_list", "job_dry_run", "job_start", "job_stop"}) {
      m[t] = Rule::kTenant;
    }
    for (const char* t : {"box_graph", "box_logs", "version_get", "version_list", "version_promote", "source_box_history"}) {
      m[t] = Rule::kBox;
    }
    for (const char* t : {"user_delete", "deletions"}) m[t] = Rule::kTenant;
    for (const char* t : {"flag_set", "flag_list"}) m[t] = Rule::kTenant;
    for (const char* t : {"box_disable", "box_enable"}) m[t] = Rule::kBox;
    for (const char* t : {"db_snapshot", "db_snapshots", "db_restore_to_new", "db_create", "db_unbound"}) m[t] = Rule::kTenant;
    m["version_upload"] = Rule::kManifest;
    for (const char* t : {"mcp_tools_preview", "route_check"}) m[t] = Rule::kDomain;
    for (const char* t : {"mail_domain_check", "mail_route_check"}) m[t] = Rule::kDomain;
    m["schema_put"] = Rule::kSchema;
    for (const char* t : {"source_build", "source_checkout", "source_commit", "source_deploy", "source_diff",
                          "source_log", "source_read", "source_revert", "source_tree"}) {
      m[t] = Rule::kProject;
    }
    m["source_stage"] = Rule::kProject;
    m["source_create"] = Rule::kProjectCreate;
    m["attachment_upload_part"] = Rule::kTicket;
    for (const char* t : {"cache_status", "cache_clear"}) m[t] = Rule::kTenant;
    for (const char* t : {"change_get", "source_job", "job_status"}) m[t] = Rule::kResult;
    for (const char* t : {"perf_report", "perf_statements"}) m[t] = Rule::kTenant;
    for (const char* t : {"domain_list", "box_list", "source_list", "timers_list", "outbox_dead_letters", "uri_stats",
                          "grant_usage"}) {
      m[t] = Rule::kFiltered;
    }
    m["whats_new"] = Rule::kOpen;
    for (const char* t : {"box_guide", "site_guide", "rust_binding", "library_list", "library_get", "schema_list",
                          "schema_get", "web_fetch", "operator_scope", "tool_schemas", "url_check", "log_search", "spec_put",
                          "spec_list", "spec_get", "approval_status"}) {
      m[t] = Rule::kOpen;
    }
    m["schema_check"] = Rule::kOpen;
    for (const char* t : {"host_health", "ids_status", "hot_boxes", "access_log", "operator_actions", "key_list", "backup_run",
                          "backup_status", "crawl_list", "crawl_pages", "crawl_records", "crawl_rerun", "crawl_start",
                          "crawl_stop", "web_optout", "log_verify", "log_status", "agent_audit"}) {
      m[t] = Rule::kHost;
    }
    return m;
  }();
  return kRules;
}

std::string text_arg(const bj::object& args, const char* k) {
  auto* v = args.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

fs::path scopes_file(const fs::path& storage) { return storage / "operations" / "scopes.json"; }

}  // namespace

bool write_operator_scope(const fs::path& storage, const std::string& key,
                          const std::optional<std::set<std::string>>& tenants, const std::string& by, int64_t now_ms,
                          std::string* why) {
  auto file = scopes_file(storage);
  bj::object all;
  if (auto text = base::read_file(file)) {
    std::string err;
    auto v = json::parse(*text, &err);
    if (!v || !v->is_object()) return *why = file.string() + " doesn't parse; fix or remove it first", false;
    all = v->as_object();
  }
  if (!tenants) {
    all.erase(key);
  } else {
    bj::array ts;
    for (const auto& t : *tenants) ts.push_back(bj::value(t));
    all[key] = bj::object{{"tenants", std::move(ts)}, {"set_by", by}, {"at_ms", now_ms}};
  }
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  if (!base::write_file_durable(file, json::pretty(all))) return *why = "couldn't write " + file.string(), false;
  return true;
}

std::optional<Host::ScopeRule> Host::scope_rule(std::string_view tool) {
  auto it = rules().find(tool);
  if (it == rules().end()) return std::nullopt;
  return it->second;
}

std::string Host::scope_key(const std::string& operator_name) {
  // A person is one operator whichever way they come: the panel, or any agent signed in as them.
  if (operator_name.starts_with("oauth:")) {
    std::string rest = operator_name.substr(6);
    return "person:" + rest.substr(0, rest.rfind(':'));
  }
  if (operator_name.starts_with("panel:")) return "person:" + operator_name.substr(6);
  return operator_name;  // "config", "token:$name"
}

std::map<std::string, Host::OperatorScope> Host::operator_scopes() const {
  std::map<std::string, OperatorScope> out;
  auto text = base::read_file(scopes_file(config_.get()->storage));
  if (!text) return out;
  std::string err;
  auto v = json::parse(*text, &err);
  if (!v || !v->is_object()) {
    std::cerr << "goober-host: storage/operations/scopes.json doesn't parse; every operator is focused on nothing\n";
    // A broken file must not open everything up: everyone named in it is held to no tenants.
    return {{"*", OperatorScope{false, {}, "", 0}}};
  }
  for (const auto& kv : v->as_object()) {
    const auto* o = kv.value().if_object();
    if (!o) continue;
    OperatorScope s;
    if (const auto* t = o->if_contains("tenants"); t && t->is_array()) {
      s.all = false;
      for (const auto& e : t->as_array()) {
        if (e.is_string()) s.tenants.insert(std::string(e.as_string()));
      }
    }
    if (const auto* b = o->if_contains("set_by"); b && b->is_string()) s.set_by = std::string(b->as_string());
    if (const auto* a = o->if_contains("at_ms"); a && a->is_int64()) s.at_ms = a->as_int64();
    out[std::string(kv.key())] = std::move(s);
  }
  return out;
}

Host::OperatorScope Host::operator_scope(const std::string& operator_name) const {
  auto all = operator_scopes();
  if (auto broken = all.find("*"); broken != all.end() && all.size() == 1 && !broken->second.all) return broken->second;
  auto it = all.find(scope_key(operator_name));
  return it == all.end() ? OperatorScope{} : it->second;
}

bool Host::set_operator_scope(const std::string& key, const OperatorScope& scope, const std::string& by,
                              std::string* why) {
  auto cfg = config_.get();
  std::set<std::string> known;
  for (const auto& [d, e] : cfg->domains) known.insert(e.tenant);
  if (!scope.all) {
    if (scope.tenants.empty()) return *why = "a scope names at least one tenant, or is all", false;
    for (const auto& t : scope.tenants) {
      if (!known.count(t)) return *why = "no domain in the host config serves tenant " + t, false;
    }
  }
  std::lock_guard lock(scopes_mu_);
  return write_operator_scope(cfg->storage, key, scope.all ? std::nullopt : std::optional(scope.tenants), by,
                              base::now_unix_ms(), why);
}

std::optional<std::string> Host::scope_refusal(const OperatorScope& scope, const std::string& operator_name,
                                               const std::string& tool, const bj::object& args) {
  if (scope.all) return std::nullopt;
  auto cfg = config_.get();
  std::string names;
  for (const auto& t : scope.tenants) names += (names.empty() ? "" : ", ") + t;
  std::string who = "operator " + operator_name + " is focused on " + (names.empty() ? "no tenant" : names);
  std::string widen = "; an owner can widen it in the panel's Operators view";
  auto in = [&](const std::string& tenant) { return scope.tenants.count(tenant) > 0; };
  auto tenant_of_domain = [&](const std::string& domain) -> std::optional<std::string> {
    std::string name = base::to_lower(domain);
    if (auto r = cfg->redirects.find(name); r != cfg->redirects.end()) name = r->second;  // its primary's tenant
    auto it = cfg->domains.find(name);
    if (it == cfg->domains.end()) return std::nullopt;
    return it->second.tenant;
  };
  // A box is in scope when every domain it names is configured and in scope; "*" is every tenant.
  // ...and every database it pins to a fixed tenant is in scope too, since a pinned strategy
  // reaches that tenant's data whatever domains the box serves.
  auto manifest_in = [&](const manifest::Manifest& m, std::string* out_of) {
    for (const auto& d : m.domains) {
      if (d == "*") return *out_of = "every tenant (\"*\")", false;
      auto t = tenant_of_domain(d);
      if (!t) return *out_of = d + " (no tenant yet)", false;
      if (!in(*t)) return *out_of = *t, false;
    }
    for (const auto& s : m.data) {
      if (!s.tenant.empty() && !in(s.tenant)) return *out_of = "tenant " + s.tenant + "'s database (data." + s.name + ")", false;
    }
    return true;
  };
  // A new version replaces the box of that name: if one exists outside the scope, it's not theirs.
  auto existing_in = [&](const std::string& name, std::string* out_of) {
    auto active = boxes_->active();
    auto it = active->boxes.find(name);
    if (it != active->boxes.end()) return manifest_in(it->second->manifest, out_of);
    return true;
  };
  auto box_in = [&](const std::string& name, std::string* out_of) {
    auto active = boxes_->active();
    auto it = active->boxes.find(name);
    if (it != active->boxes.end()) return manifest_in(it->second->manifest, out_of);
    // Versions not active: the newest stored manifest decides.
    auto versions = boxes_->versions(name);
    if (!versions.empty()) {
      std::string err;
      auto text = boxes_->manifest_text(name, versions.back().version);
      auto m = text ? manifest::parse_manifest(*text, &err) : std::nullopt;
      if (m) return manifest_in(*m, out_of);
    }
    return *out_of = "box " + name + ", which this host doesn't have", false;
  };

  auto rule = scope_rule(tool);
  if (!rule) return who + ", and " + tool + " has no scope rule, so it's refused" + widen;
  switch (*rule) {
    case Rule::kOpen:
    case Rule::kFiltered:
    case Rule::kResult:
      return std::nullopt;
    case Rule::kHost:
      return who + ", and " + tool + " reads or changes the whole host" + widen;
    case Rule::kTenant: {
      std::string t = text_arg(args, "tenant");
      if (t.empty()) return who + ": name the tenant (" + tool + " without one means every tenant)";
      if (!in(t)) return who + ", not " + t + widen;
      // Keys act on their scope, and a system key is every tenant's.
      if ((tool == "key_set" || tool == "key_delete") && text_arg(args, "scope") != "tenant") {
        return who + ", and a system key is every tenant's: only scope \"tenant\" is theirs" + widen;
      }
      return std::nullopt;
    }
    case Rule::kBox: {
      std::string out_of;
      if (!box_in(text_arg(args, "box"), &out_of)) return who + ", and box " + text_arg(args, "box") + " serves " + out_of + widen;
      // Promoting makes that version's manifest the one that runs, so it is the one checked.
      if (tool == "version_promote" || tool == "box_enable") {
        std::string err;
        std::string version = text_arg(args, "version");
        if (version.empty() && tool == "box_enable") {
          if (auto d = boxes_->disabled(text_arg(args, "box"))) version = d->version;
        }
        auto text = boxes_->manifest_text(text_arg(args, "box"), version);
        auto m = text ? manifest::parse_manifest(*text, &err) : std::nullopt;
        if (!m) return std::nullopt;  // the tool says there's no such version
        if (!manifest_in(*m, &out_of)) return who + ", and that version of " + text_arg(args, "box") + " serves " + out_of + widen;
      }
      return std::nullopt;
    }
    case Rule::kManifest: {
      const auto* mv = args.if_contains("manifest");
      std::string text = mv && mv->is_string() ? std::string(mv->as_string()) : mv ? json::dump(*mv) : "";
      std::string err;
      auto m = manifest::parse_manifest(text, &err);
      if (!m) return std::nullopt;  // the tool itself says what's wrong with it
      std::string out_of;
      if (!manifest_in(*m, &out_of)) return who + ", and this box would serve " + out_of + widen;
      if (!existing_in(m->box, &out_of)) return who + ", and box " + m->box + " already serves " + out_of + widen;
      return std::nullopt;
    }
    case Rule::kDomain: {
      std::string d = text_arg(args, "domain");
      auto t = tenant_of_domain(d);
      if (!t || !in(*t)) return who + ", and " + (d.empty() ? std::string("no domain") : d) + " isn't one of theirs" + widen;
      return std::nullopt;
    }
    case Rule::kSchema: {
      std::string name;
      if (const auto* s = args.if_contains("schema"); s && s->is_object()) {
        if (const auto* n = s->as_object().if_contains("schema"); n && n->is_string()) name = std::string(n->as_string());
      } else if (s && s->is_string()) {
        std::string err;
        if (auto parsed = json::parse(s->as_string(), &err); parsed && parsed->is_object()) {
          if (const auto* n = parsed->as_object().if_contains("schema"); n && n->is_string()) name = std::string(n->as_string());
        }
      }
      auto active = boxes_->active();
      for (const auto& [box_name, box] : active->boxes) {
        bool binds = false;
        for (const auto& d : box->manifest.data) binds |= d.schema == name;
        std::string out_of;
        if (binds && !manifest_in(box->manifest, &out_of)) {
          return who + ", and schema " + name + " is bound by box " + box_name + ", which serves " + out_of + widen;
        }
      }
      return std::nullopt;
    }
    case Rule::kProjectCreate: {
      std::string t = text_arg(args, "tenant");
      if (!t.empty() && !in(t)) return who + ", not " + t + widen;
      return std::nullopt;
    }
    case Rule::kProject: {
      std::string project = text_arg(args, "project");
      auto p = source_->get(project);
      if (!p) return std::nullopt;  // the tool says there's no such project
      int64_t rev = p->head;
      if (tool == "source_deploy" || tool == "source_build") {
        if (const auto* r = args.if_contains("rev"); r && r->is_int64() && r->as_int64() > 0) rev = r->as_int64();
      }
      // A commit is checked as the project will be after it: the files it writes, over the head.
      const bj::array* changes = nullptr;
      if (tool == "source_commit") {
        if (const auto* ch = args.if_contains("changes"); ch && ch->is_array()) changes = &ch->as_array();
      }
      std::string err;
      auto read = [&](const std::string& path) -> std::optional<std::string> {
        if (changes) {
          for (const auto& ch : *changes) {
            const auto* o = ch.if_object();
            if (!o || text_arg(*o, "path") != path) continue;
            if (const auto* d = o->if_contains("delete"); d && d->is_bool() && d->as_bool()) return std::nullopt;
            if (const auto* t = o->if_contains("text"); t && t->is_string()) return std::string(t->as_string());
            if (const auto* b = o->if_contains("base64"); b && b->is_string()) return base::base64_decode(b->as_string());
          }
        }
        if (rev == 0) return std::nullopt;
        return source_->read(project, rev, path, &err);
      };
      if (rev == 0 && !changes) return std::nullopt;  // nothing committed yet: nothing to reach
      auto text = read("goober.project.json");
      if (!text) return std::nullopt;
      auto spec = read_spec(*text, &err);
      if (!spec) return std::nullopt;  // the tool says what's wrong with it
      for (const auto& [dir, tenant] : spec->sites) {
        if (!in(tenant)) return who + ", and project " + project + " writes tenant " + tenant + "'s site" + widen;
      }
      for (const auto& dir : spec->boxes) {
        auto mtext = read(dir == "." ? "manifest.json" : dir + "/manifest.json");
        if (!mtext) continue;
        auto m = manifest::parse_manifest(*mtext, &err);
        std::string out_of;
        if (m && !manifest_in(*m, &out_of)) {
          return who + ", and project " + project + "'s box " + m->box + " serves " + out_of + widen;
        }
        if (m && !existing_in(m->box, &out_of)) {
          return who + ", and project " + project + "'s box " + m->box + " would replace one serving " + out_of + widen;
        }
      }
      return std::nullopt;
    }
    case Rule::kTicket: {
      std::string ticket = text_arg(args, "ticket");
      if (auto at = ticket.find("ticket="); at != std::string::npos) ticket = ticket.substr(at + 7);
      ticket = ticket.substr(0, ticket.find_first_of("&# "));
      auto t = upload_ticket_tenant(ticket);
      if (t && !in(*t)) return who + ", not " + *t + widen;
      return std::nullopt;
    }
  }
  return who + widen;
}

std::optional<std::string> Host::scope_filter(const OperatorScope& scope, const std::string& operator_name,
                                              const std::string& tool, bj::value& result) {
  if (scope.all) return std::nullopt;
  auto rule = scope_rule(tool);
  if (!rule || (*rule != Rule::kFiltered && *rule != Rule::kResult)) return std::nullopt;
  auto cfg = config_.get();
  auto in = [&](const std::string& tenant) { return scope.tenants.count(tenant) > 0; };
  auto domains_in = [&](const bj::array& domains) {
    for (const auto& d : domains) {
      if (!d.is_string()) return false;
      auto it = cfg->domains.find(std::string(d.as_string()));
      if (it == cfg->domains.end() || !in(it->second.tenant)) return false;
    }
    return true;
  };
  auto project_in = [&](const std::string& project) {
    return !scope_refusal(scope, operator_name, "source_read", bj::object{{"project", project}});
  };
  auto row_in = [&](const bj::object& row) {
    if (const auto* t = row.if_contains("tenant"); t && t->is_string()) return in(std::string(t->as_string()));
    if (const auto* d = row.if_contains("domains"); d && d->is_array()) return domains_in(d->as_array());
    if (const auto* p = row.if_contains("project"); p && p->is_string()) return project_in(std::string(p->as_string()));
    if (const auto* b = row.if_contains("box"); b && b->is_string()) {
      return !scope_refusal(scope, operator_name, "box_logs", bj::object{{"box", b->as_string()}});
    }
    return false;  // a row that says nothing about where it belongs isn't shown
  };
  if (*rule == Rule::kFiltered) {
    if (!result.is_array()) return std::nullopt;
    bj::array kept;
    for (auto& row : result.as_array()) {
      if (row.is_object() && row_in(row.as_object())) kept.push_back(std::move(row));
    }
    result = std::move(kept);
    return std::nullopt;
  }
  // kResult: one record, shown only when it belongs to the scope; a list of them is filtered.
  if (result.is_array()) {
    bj::array kept;
    for (auto& row : result.as_array()) {
      if (row.is_object() && row_in(row.as_object())) kept.push_back(std::move(row));
    }
    result = std::move(kept);
    return std::nullopt;
  }
  if (result.is_object() && row_in(result.as_object())) return std::nullopt;
  std::string names;
  for (const auto& t : scope.tenants) names += (names.empty() ? "" : ", ") + t;
  return "operator " + operator_name + " is focused on " + names + ", and this " +
         (tool == "change_get" ? "change" : tool == "job_status" ? "run" : "job") + " is outside it";
}

mcp::McpServer::Policy Host::operator_policy(const std::string& operator_name, bool read_only, bool hold) {
  mcp::McpServer::Policy policy;
  if (hold && approvals_on(operator_name)) {
    policy.hold = [this, operator_name](const std::string& tool, const bj::object& args) {
      return hold_for_approval(operator_name, tool, args);
    };
  }
  OperatorScope scope = operator_scope(operator_name);
  // A tenant left out is filled in when only one is possible: the host serves one tenant, or the
  // operator is focused on one (031 § 5).
  {
    std::set<std::string> possible;
    for (const auto& [d, e] : config_.get()->domains) {
      if (scope.all || scope.tenants.count(e.tenant)) possible.insert(e.tenant);
    }
    if (possible.size() == 1) {
      policy.prepare = [only = *possible.begin()](const mcp::Tool& tool, bj::object& args) {
        // Only where a tenant is required: a tool whose tenant is optional means something by its
        // absence (every tenant, the system's keys, a box project).
        bool required = false;
        if (auto* req = tool.input_schema.if_contains("required"); req && req->is_array()) {
          for (const auto& r : req->as_array()) required |= r.is_string() && r.as_string() == "tenant";
        }
        if (required && !args.contains("tenant")) args["tenant"] = only;
      };
    }
  }
  // A deploy or build without a revision is pinned to the head before the scope check, so the
  // revision checked is the one that runs (a commit landing in between can't slip past it).
  {
    auto fill_tenant = policy.prepare;
    policy.prepare = [this, fill_tenant](const mcp::Tool& tool, bj::object& args) {
      if (fill_tenant) fill_tenant(tool, args);
      if ((tool.name == "source_deploy" || tool.name == "source_build") && !args.contains("rev")) {
        if (auto p = source_->get(text_arg(args, "project")); p && p->head > 0) args["rev"] = p->head;
      }
    };
  }
  if (read_only || !scope.all) {
    policy.allowed = [read_only, focused = !scope.all](const std::string& tool) {
      if (read_only && !is_read_tool(tool)) return false;
      if (focused) {
        auto rule = scope_rule(tool);
        return rule && *rule != ScopeRule::kHost;
      }
      return true;
    };
  }
  if (read_only || !scope.all) {
    policy.check = [this, scope, operator_name, read_only](const std::string& tool, const bj::object& args) {
      // A read tool can still change something with some arguments (mail_unsubscribed's "set").
      if (read_only && (!is_read_tool(tool) || !is_read_call(tool, args))) {
        return std::optional<std::string>("its access is read-only");
      }
      if (scope.all) return std::optional<std::string>();
      return scope_refusal(scope, operator_name, tool, args);
    };
  }
  if (!scope.all) {
    policy.filter = [this, scope, operator_name](const std::string& tool, bj::value& result) {
      return scope_filter(scope, operator_name, tool, result);
    };
  }
  return policy;
}

}  // namespace goober::host

namespace goober::host {

bj::object Host::panel_operators(const SignedIn& who) {
  auto cfg = config_.get();
  auto scopes = operator_scopes();
  auto scope_json = [&](const std::string& key) -> bj::value {
    auto it = scopes.find(key);
    if (it == scopes.end() || it->second.all) return "all";
    bj::array ts;
    for (const auto& t : it->second.tenants) ts.push_back(bj::value(t));
    return bj::object{{"tenants", std::move(ts)}, {"set_by", it->second.set_by}, {"at_ms", it->second.at_ms}};
  };
  bj::array out;
  for (const auto& [email, roles] : cfg->identity.operators) {
    bj::array rs;
    for (const auto& r : roles) rs.push_back(bj::value(r));
    out.push_back(bj::object{{"key", "person:" + email}, {"kind", "person"}, {"name", email}, {"roles", std::move(rs)},
                             {"scope", scope_json("person:" + email)}, {"you", email == who.user.email}});
  }
  for (const auto& t : list_operator_tokens(cfg->storage)) {
    out.push_back(bj::object{{"key", "token:" + t.name}, {"kind", "token"}, {"name", t.name},
                             {"roles", bj::array{"host:owner"}}, {"scope", scope_json("token:" + t.name)}, {"you", false}});
  }
  if (!cfg->operations_token.empty()) {
    out.push_back(bj::object{{"key", "config"}, {"kind", "config"}, {"name", "the config's operations-token"},
                             {"roles", bj::array{"host:owner"}}, {"scope", scope_json("config")}, {"you", false}});
  }
  bool owner = std::find(who.roles.begin(), who.roles.end(), "host:owner") != who.roles.end();
  return bj::object{{"operators", std::move(out)}, {"may_set_any", owner}};
}

std::pair<int, bj::object> Host::panel_set_scope(const SignedIn& who, const bj::object& args, const std::string& request_id) {
  auto has = [&](const char* role) { return std::find(who.roles.begin(), who.roles.end(), role) != who.roles.end(); };
  bool owner = has("host:owner"), op = has("host:operator");
  if (!owner && !op) return {403, bj::object{{"error", "an observer can't change a scope"}}};
  const auto* c = args.if_contains("confirm");
  if (!c || !c->is_bool() || !c->as_bool()) return {400, bj::object{{"error", "confirm the change: {\"confirm\": true}"}}};
  std::string key = text_arg(args, "operator");
  std::string mine = "person:" + who.user.email;
  OperatorScope next;
  const auto* t = args.if_contains("tenants");
  if (t && t->is_string() && t->as_string() == "all") {
    next.all = true;
  } else if (t && t->is_array()) {
    next.all = false;
    for (const auto& e : t->as_array()) {
      if (!e.is_string()) return {400, bj::object{{"error", "tenants is \"all\" or a list of tenant names"}}};
      next.tenants.insert(std::string(e.as_string()));
    }
  } else {
    return {400, bj::object{{"error", "tenants is \"all\" or a list of tenant names"}}};
  }
  auto scopes = operator_scopes();
  auto it = scopes.find(key);
  OperatorScope before = it == scopes.end() ? OperatorScope{} : it->second;
  // An operator narrows only their own scope, and never widens it (030 § 2).
  if (!owner) {
    if (key != mine) return {403, bj::object{{"error", "only a host:owner sets another operator's scope"}}};
    bool narrower = !next.all && (before.all || std::includes(before.tenants.begin(), before.tenants.end(),
                                                              next.tenants.begin(), next.tenants.end()));
    if (!narrower) return {403, bj::object{{"error", "you may only narrow your own scope; an owner widens it"}}};
  }
  bool known = key == "config" || key.starts_with("token:") || key.starts_with("person:");
  if (!known) return {400, bj::object{{"error", "operator is person:$email, token:$name, or config"}}};
  std::string why;
  if (!set_operator_scope(key, next, "panel:" + who.user.email, &why)) return {400, bj::object{{"error", why}}};
  auto as_json = [](const OperatorScope& s) -> bj::value {
    if (s.all) return "all";
    bj::array ts;
    for (const auto& x : s.tenants) ts.push_back(bj::value(x));
    return ts;
  };
  if (changes_) {
    try {
      changes_->record_action(operations::Action{base::now_unix_ms(), "panel:" + who.user.email, "operator_scope_set",
                                                 request_id,
                                                 bj::object{{"operator", key}, {"from", as_json(before)}, {"to", as_json(next)}},
                                                 false});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  }
  return {200, bj::object{{"operator", key}, {"scope", as_json(next)}}};
}

void Host::register_scope_tools() {
  mcp_->add_tool({"operator_scope",
                  "Read tool. Your own operator scope: scope is \"all\", or the list of tenants you're focused on. A "
                  "tool aimed at another tenant is refused, and lists show only your tenants. Only a person changes it, "
                  "in the panel's Operators view; no tool can.",
                  mcp::object_schema({}, {}), [this](const bj::object&) {
                    std::string me = current_operator();
                    auto s = operator_scope(me);
                    bj::value scope = "all";
                    if (!s.all) {
                      bj::array ts;
                      for (const auto& t : s.tenants) ts.push_back(bj::value(t));
                      scope = std::move(ts);
                    }
                    return mcp::ToolResult::ok(bj::object{{"operator", me}, {"key", scope_key(me)}, {"scope", std::move(scope)}});
                  }});
}

}  // namespace goober::host

