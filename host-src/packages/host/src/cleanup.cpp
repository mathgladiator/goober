// Cleanup (design/machine/051.Cleanup.md): dropping a box for good, and asking for drops. An
// agent's box_drop and db_drop always wait for a person, whatever its approvals switch says, and
// only a host:owner approves one, in the panel's Cleanup view, typing the name of what goes; a
// person can also drop there directly. A dry run says what a drop would take and what stops it.
// The database side of the checks and drops is db_admin.cpp's.

#include <iostream>
#include <set>

#include "goober/base/files.h"
#include "goober/base/names.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

bool flag_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_bool() && v->as_bool();
}

int64_t bytes_under(const fs::path& dir) {
  int64_t n = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    std::error_code e2;
    if (it->is_regular_file(e2)) n += int64_t(it->file_size(e2));
  }
  return n;
}

std::string joined(const bj::array& a) {
  std::string out;
  for (const auto& v : a) out += (out.empty() ? "" : "; ") + std::string(v.as_string());
  return out;
}

}  // namespace

bool Host::always_held(std::string_view tool, const bj::object& args) {
  return (tool == "box_drop" || tool == "db_drop") && !flag_of(args, "dry_run");
}

std::string Host::drop_approval(std::string_view tool, const bj::object& args) {
  if (!approved_by_now().empty()) return approved_by_now();
  // Development mode (--local, `just run`): drops don't wait (the human, 2026-11-04).
  if (config_.get()->local) return "development mode (--local)";
  // Beta's operator drops what is beta's alone without a wait (053 § 6); the rest waits for an owner.
  if (operator_scope(current_operator()).beta && beta_drop_free(tool, args)) return "beta's operator (053 § 6)";
  return "";
}

bool Host::beta_only_box(const std::string& box) {
  if (!base::is_valid_name(box) || boxes_->active()->boxes.count(box) || boxes_->disabled(box)) return false;
  if (!boxes_->beta_version(box)) return false;
  // Every version first stored for beta, and none ever live: production never had it.
  for (const auto& v : boxes_->versions(box)) {
    if (v.state == "deleted") continue;
    if (!v.for_beta || !v.live.empty()) return false;
  }
  return true;
}

bool Host::beta_drop_free(std::string_view tool, const bj::object& args) {
  if (tool == "box_drop") return beta_only_box(text_of(args, "box"));
  if (tool == "db_drop") return config_.get()->is_beta_tenant(text_of(args, "tenant"));
  return false;
}

std::string& Host::approved_by_now() {
  thread_local std::string who;
  return who;
}

bj::object Host::box_drop_check(const std::string& box) {
  bj::object out{{"box", box}};
  bj::array blocked;
  auto cfg = config_.get();
  if (!base::is_valid_name(box) || !fs::is_directory(cfg->storage / "boxes" / box)) {
    out["state"] = "none";
    out["blocked"] = bj::array{bj::value("no box named " + box)};
    return out;
  }
  if (auto d = boxes_->dropped(box)) {
    out["state"] = "dropped";
    out["dropped"] = *d;
    out["blocked"] = bj::array{bj::value("box " + box + " was dropped already")};
    return out;
  }
  auto active = boxes_->active();
  std::optional<manifest::Manifest> m;
  std::string state = "stored", version;
  if (auto it = active->boxes.find(box); it != active->boxes.end()) {
    state = "live";
    version = it->second->version;
    m = it->second->manifest;
  } else if (auto d = boxes_->disabled(box)) {
    state = "disabled";
    version = d->version;
  } else if (auto it = active->beta.find(box); it != active->beta.end()) {
    // Only in beta (053 § 3): it answers there, so dropping it takes it out of beta at once.
    state = "beta";
    version = it->second->version;
    m = it->second->manifest;
  }
  int64_t stored = 0, newest_ms = -1;
  std::string newest;
  for (const auto& v : boxes_->versions(box)) {
    if (v.state == "deleted") continue;
    ++stored;
    if (v.stored_ms > newest_ms) newest_ms = v.stored_ms, newest = v.version;
  }
  if (!m) {
    std::string err;
    std::string from = version.empty() ? newest : version;
    if (auto text = from.empty() ? std::nullopt : boxes_->manifest_text(box, from)) m = manifest::parse_manifest(*text, &err);
    if (version.empty()) version = from;
  }
  out["state"] = state;
  out["version"] = version.empty() ? bj::value(nullptr) : bj::value(version);
  out["versions"] = stored;
  out["bytes"] = bytes_under(cfg->storage / "boxes" / box / "versions");
  if (m) {
    out["mission"] = m->mission;
    bj::array domains;
    for (const auto& d : m->domains) domains.push_back(bj::value(d));
    out["domains"] = std::move(domains);
    out["routes"] = int64_t(m->routes.size() + m->tools.size() + m->internal.size() + m->mail.size() + m->timers.size());
    // Its databases stay: each is left bound by another box, or unbound, for db_drop after.
    bj::array dbs;
    std::set<std::string> seen;
    for (const auto& [tenant, domain] : tenants_of(*cfg, *m)) {
      for (const auto& d : m->data) {
        if (!d.applies_schema() || (!d.tenant.empty() && d.tenant != tenant)) continue;
        std::string database = d.database_for(tenant);
        if (!seen.insert(tenant + "/" + database).second || !databases_->exists(tenant, database)) continue;
        bj::array others;
        for (const auto& b : db_bindings(tenant, database)) {
          if (b.box != box) others.push_back(bj::value(b.box));
        }
        dbs.push_back(bj::object{{"tenant", tenant},
                                 {"database", database},
                                 {"strategy", d.name},
                                 {"after", others.empty() ? "unbound: db_drop it after, if no one needs it" : "still bound by other boxes"},
                                 {"also_bound_by", std::move(others)}});
      }
    }
    out["databases"] = std::move(dbs);
  }
  // A box that calls this one (live or disabled, so it may be enabled again; or its beta version)
  // would break.
  bj::array callers;
  for (const auto& name : boxes_->box_names()) {
    if (name == box) continue;
    std::vector<std::pair<manifest::Manifest, std::string>> others;  // a version, and which
    if (auto it = active->boxes.find(name); it != active->boxes.end()) {
      others.emplace_back(it->second->manifest, "");
    } else if (auto d = boxes_->disabled(name)) {
      std::string err;
      auto text = boxes_->manifest_text(name, d->version);
      if (auto m = text ? manifest::parse_manifest(*text, &err) : std::nullopt) others.emplace_back(std::move(*m), "");
    }
    if (auto it = active->beta.find(name); it != active->beta.end()) others.emplace_back(it->second->manifest, "beta version of ");
    bool listed = false;
    for (const auto& [other, which] : others) {
      for (const auto& svc : other.external) {
        if (svc.box != box) continue;
        if (!listed) callers.push_back(bj::value(name));
        listed = true;
        blocked.push_back(bj::value("the " + which + "box " + name + " calls it (external." + svc.name + "): drop " + name +
                                    " first, or deploy a version of it that no longer does"));
      }
    }
  }
  out["callers"] = std::move(callers);
  // Left behind, said so: controls on its API (inert once it goes; control_remove them).
  bj::array controls;
  std::error_code ec;
  for (const auto& tenant_dir : fs::directory_iterator(cfg->storage / "controls", ec)) {
    std::error_code e2;
    for (const auto& f : fs::directory_iterator(tenant_dir.path(), e2)) {
      auto text = base::read_file(f.path());
      if (text && text->find("\"box:" + box + "\"") != std::string::npos) controls.push_back(bj::value(f.path().stem().string()));
    }
  }
  out["controls_on_domains"] = std::move(controls);
  out["takes"] = std::string(state == "live" ? "out of service at once; " : state == "beta" ? "out of beta at once; " : "") +
                 "every stored version, each leaving a tombstone (version_get shows it); its flags. Its databases, "
                 "logs, and change log rows stay. The name can be uploaded again as a new box.";
  out["blocked"] = std::move(blocked);
  return out;
}

std::pair<int, bj::object> Host::drop_box(const std::string& box, const std::string& by, const std::string& why,
                                          const std::string& approved_by, const std::string& request_id) {
  auto check = box_drop_check(box);
  if (!check.at("blocked").as_array().empty()) {
    return {409, bj::object{{"error", box + " can't be dropped now: " + joined(check.at("blocked").as_array())}, {"check", check}}};
  }
  bj::object record{{"by", by}, {"why", why.substr(0, 500)}, {"was", check.at("state")}, {"version", check.at("version")}};
  if (!approved_by.empty()) record["approved_by"] = approved_by;
  std::string err;
  auto gone = boxes_->drop(box, record, &err);
  if (!gone) return {400, bj::object{{"error", err}}};
  // Its flags, for every tenant.
  wal::Bundle flags;
  std::error_code ec;
  for (const auto& tenant_dir : fs::directory_iterator(config_.get()->storage / "flags", ec)) {
    std::string rel = "flags/" + tenant_dir.path().filename().string() + "/" + box + ".json";
    std::error_code e2;
    if (fs::exists(config_.get()->storage / rel, e2)) flags.push_back({wal::FileChange::Kind::kDelete, rel, ""});
  }
  if (!flags.empty() && !wal_->commit(flags, &err)) std::cerr << "goober-host: dropping " << box << "'s flags: " << err << "\n";
  bj::array versions;
  for (const auto& v : *gone) versions.push_back(bj::value(v));
  bj::object detail{{"box", box}, {"was", check.at("state")}, {"versions", versions}, {"why", why.substr(0, 500)}};
  if (!approved_by.empty()) detail["approved_by"] = approved_by;
  if (changes_) {
    try {
      changes_->record_action(operations::Action{base::now_unix_ms(), by, "box_drop", request_id, detail, false});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  }
  audit("drop.box", by, "box:" + box, "ok", detail);
  bj::array left;
  if (const auto* dbs = check.if_contains("databases"); dbs && dbs->is_array()) {
    for (const auto& d : dbs->as_array()) {
      if (d.at("also_bound_by").as_array().empty()) left.push_back(bj::object{{"tenant", d.at("tenant")}, {"database", d.at("database")}});
    }
  }
  return {200, bj::object{{"dropped", box},
                          {"versions", std::move(versions)},
                          {"databases_unbound", std::move(left)},
                          {"controls_on_domains", check.at("controls_on_domains")}}};
}

bj::object Host::cleanup_report() {
  auto cfg = config_.get();
  bj::array boxes, dropped;
  for (const auto& name : boxes_->box_names()) {
    if (auto d = boxes_->dropped(name)) {
      dropped.push_back(bj::object{{"box", name},
                                   {"dropped_ms", d->contains("dropped_ms") ? d->at("dropped_ms") : bj::value(nullptr)},
                                   {"by", d->contains("by") ? d->at("by") : bj::value(nullptr)},
                                   {"approved_by", d->contains("approved_by") ? d->at("approved_by") : bj::value(nullptr)},
                                   {"why", d->contains("why") ? d->at("why") : bj::value("")}});
      continue;
    }
    boxes.push_back(box_drop_check(name));
  }
  std::set<std::string> tenants;
  for (const auto& [d, e] : cfg->domains) tenants.insert(e.tenant);
  bj::array dbs;
  for (const auto& tenant : tenants) {
    for (const auto& database : databases_->databases(tenant)) dbs.push_back(db_drop_check(tenant, database, ""));
  }
  bj::array snaps;
  for (const auto& tenant : tenants) {
    auto list = list_snapshots(tenant, "");
    int64_t bytes = 0;
    for (const auto& s : list) {
      if (const auto* b = s.if_contains("bytes"); b && b->is_int64()) bytes += b->as_int64();
    }
    if (!list.empty()) snaps.push_back(bj::object{{"tenant", tenant}, {"snapshots", int64_t(list.size())}, {"bytes", bytes}});
  }
  return bj::object{{"boxes", std::move(boxes)}, {"databases", std::move(dbs)}, {"snapshots", std::move(snaps)},
                    {"dropped", std::move(dropped)}};
}

void Host::register_cleanup_tools() {
  using mcp::boolean_property;
  using mcp::object_schema;
  using mcp::string_property;
  const std::string waits =
      " Without dry_run it always waits for a person, whatever your approvals switch says: it answers pending_approval "
      "{id}, a host:owner approves or denies it in the panel's Cleanup view (typing the name of what goes), and "
      "approval_status {id} gives the outcome. The checks run again when it is approved.";

  mcp_->add_tool(
      {"box_drop",
       "Asks to drop a box for good: out of service if it is live, every stored version deleted (each leaves a "
       "tombstone), its flags removed. Its databases, logs, and change log rows stay; a database it alone bound is "
       "left unbound, for db_drop after. Refused while another box calls it. The name can be uploaded again as a new "
       "box. dry_run: true changes nothing and answers what it would take and what blocks it (state, versions, bytes, "
       "databases, callers, controls left on its API)." + waits,
       object_schema({{"box", string_property("The box to drop.")},
                      {"why", string_property("Why it goes, shown to the person who decides and kept in the change log.")},
                      {"dry_run", boolean_property("Only check: what it would take and what blocks it.")}},
                     {"box"}),
       [this](const bj::object& args) {
         std::string box = text_of(args, "box");
         if (flag_of(args, "dry_run")) return ToolResult::ok(box_drop_check(box));
         std::string approved = drop_approval("box_drop", args);
         if (approved.empty()) return ToolResult::fail("a drop runs only once a person approves it in the panel");
         auto [status, body] = drop_box(box, current_operator(), text_of(args, "why"), approved, current_request_id());
         if (status != 200) return ToolResult::fail(text_of(body, "error"));
         return ToolResult::ok(std::move(body));
       }});

  mcp_->add_tool(
      {"db_drop",
       "Asks to drop a tenant's database, or one table of it with `table`: a snapshot first (kept until a person "
       "deletes it), then the database or table and its rows. Refused while a box (live or disabled) binds it: drop "
       "the box first. dry_run: true changes nothing and answers what it would take (bytes, tables and rows) and what "
       "blocks it." + waits,
       object_schema({{"tenant", string_property("The tenant.")},
                      {"database", string_property("The database, like data.")},
                      {"table", string_property("Only this table.")},
                      {"why", string_property("Why it goes, shown to the person who decides and kept in the change log.")},
                      {"dry_run", boolean_property("Only check: what it would take and what blocks it.")}},
                     {"tenant", "database"}),
       [this](const bj::object& args) {
         std::string tenant = text_of(args, "tenant"), database = text_of(args, "database"), table = text_of(args, "table");
         if (flag_of(args, "dry_run")) return ToolResult::ok(db_drop_check(tenant, database, table));
         std::string approved = drop_approval("db_drop", args);
         if (approved.empty()) return ToolResult::fail("a drop runs only once a person approves it in the panel");
         auto [status, body] = drop_database(tenant, database, table, current_operator(), text_of(args, "why"),
                                             approved, current_request_id());
         if (status != 200) return ToolResult::fail(text_of(body, "error"));
         return ToolResult::ok(std::move(body));
       }});

  mcp_->add_tool(
      {"cleanup_report",
       "Read tool. What could be cleaned up on the host: every box not dropped (state live, disabled, or stored; "
       "versions, bytes, databases, callers, and what blocks dropping it), every tenant's databases (bytes, tables "
       "and rows, bound_by, blocked), snapshots per tenant, and the boxes dropped (when, by, approved_by, why). "
       "Ask for drops with box_drop and db_drop; a person decides.",
       object_schema({}, {}), [this](const bj::object&) { return ToolResult::ok(cleanup_report()); }});
}

}  // namespace goober::host
