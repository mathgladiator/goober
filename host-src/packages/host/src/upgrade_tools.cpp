// Upgrade boxes (design/human/420.Migration.md; design/machine/041.SchemaEvolution.md § 7): a
// schema step the host can't emulate, carried over row by row by a pure box an operator runs,
// first on copies of every database holding the old version, then for real, snapshot first.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {

std::string text_arg(const bj::object& args, const char* key) {
  auto* v = args.if_contains(key);
  return v && v->is_string() ? std::string(v->as_string()) : "";
}

std::string random_id() {
  uint8_t rnd[8];
  base::os_random_fill(rnd);
  return base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
}

}  // namespace

void Host::register_upgrade_tools() {
  // What an upgrade needs, checked by both tools: the box, its step, and that no box pinned to
  // the old version (or older) still uses a table the step rewrites. Fails with why.
  struct Plan {
    std::shared_ptr<const boxes::ActiveBox> box;
    db::Schema from, to;
    db::Step step;
    std::map<std::string, const manifest::HttpRoute*> routes;  // by table
    std::vector<std::pair<std::string, std::string>> databases;  // tenant, database holding from, not yet to
  };
  auto plan = [this](const bj::object& args, std::string* error) -> std::optional<Plan> {
    std::string name = text_arg(args, "box");
    auto active = boxes_->active();
    auto it = active->boxes.find(name);
    if (it == active->boxes.end()) return *error = "no active box named " + name, std::nullopt;
    Plan p;
    p.box = it->second;
    const auto& m = p.box->manifest;
    if (m.upgrades.empty()) return *error = "box " + name + " has no routes.upgrade: it isn't an upgrade box", std::nullopt;
    auto to = schemas_->get(m.upgrades[0].upgrade_to);
    auto from = schemas_->get(m.upgrades[0].upgrade_from);
    if (!to || !from) {
      return *error = "schemas " + m.upgrades[0].upgrade_from + " and " + m.upgrades[0].upgrade_to + " must both be stored",
             std::nullopt;
    }
    if (to->follows != from->name) {
      return *error = to->name + " doesn't follow " + from->name + "; an upgrade is one step, from the version just before",
             std::nullopt;
    }
    p.from = *from;
    p.to = *to;
    p.step = db::plan_step(*from, *to);
    for (const auto& r : m.upgrades) p.routes[r.upgrade_table] = &r;
    for (const auto& t : p.step.forced_tables) {
      if (!p.routes.count(t)) return *error = to->name + " forces table " + t + ", and box " + name + " has no routes.upgrade." + t, std::nullopt;
    }
    for (const auto& [t, r] : p.routes) {
      if (!p.step.forced_tables.count(t)) {
        return *error = "routes.upgrade." + t + ": " + to->name + " doesn't force table " + t +
                        ", so the host carries it over itself; an upgrade route is only for a forced table",
               std::nullopt;
      }
    }
    // 420's "old readers are covered, or gone", met by "gone": v1 boxes write SQL, so there are no
    // named writes or reads to translate (041 § 7).
    std::set<std::string> older{from->name};
    for (auto s = from; s && !s->follows.empty(); s = schemas_->get(s->follows)) {
      if (!older.insert(s->follows).second) break;
    }
    std::string users;
    for (const auto& box_name : boxes_->box_names()) {
      std::optional<manifest::Manifest> bm;
      if (auto a = active->boxes.find(box_name); a != active->boxes.end()) {
        bm = a->second->manifest;
      } else if (auto d = boxes_->disabled(box_name)) {
        std::string err;
        if (auto text = boxes_->manifest_text(box_name, d->version)) bm = manifest::parse_manifest(*text, &err);
      }
      if (!bm) continue;
      for (const auto& d : bm->data) {
        if (!older.count(d.schema)) continue;
        auto s = schemas_->get(d.schema);
        for (const auto& t : p.step.forced_tables) {
          if (d.tables.count(t) || (s && s->table(t))) users += (users.empty() ? "" : ", ") + box_name + "." + d.name + " (" + d.schema + ")";
        }
      }
    }
    if (!users.empty()) {
      return *error = "boxes pinned to " + from->name + " or older still use the tables " + to->name + " rewrites: " + users +
                      ". Boxes write SQL, so their writes can't pass through the upgrade box: disable them (box_disable) "
                      "or move them to " + to->name + " first (041 § 7)",
             std::nullopt;
    }
    for (const auto& tenant : databases_->tenants()) {
      for (const auto& database : databases_->databases(tenant)) {
        auto applied = databases_->applied_schemas(tenant, database);
        bool has_from = std::find(applied.begin(), applied.end(), from->name) != applied.end();
        bool has_to = std::find(applied.begin(), applied.end(), to->name) != applied.end();
        if (has_from && !has_to) p.databases.emplace_back(tenant, database);
      }
    }
    return p;
  };
  // Each forced table's handler, calling the box once per row with no clock and a fixed seed.
  auto handlers = [this](const Plan& p, const std::string& tenant) {
    std::map<std::string, db::RowTransform> out;
    std::string request_id = random_id();
    for (const auto& [table, route] : p.routes) {
      out[table] = [this, box = p.box, route = route, tenant, request_id](const bj::object& row,
                                                                        std::string* why) -> std::optional<bj::object> {
        bj::object doc{{"request_id", request_id}, {"method", "upgrade"}, {"path", "upgrade:" + route->upgrade_table},
                       {"params", bj::object{}},   {"query", bj::object{}},  {"headers", bj::object{}},
                       {"body", json::dump(row)},  {"domain", ""},           {"tenant", tenant},
                       {"user", nullptr}};
        CallOptions options;
        options.timeout_ms = route->timeout_ms;
        options.pure = true;
        auto out = call_box(boxes::RouteRef{box, route}, doc, tenant, request_id, std::nullopt, options);
        if (out.result.outcome != runtime::wasm::CallResult::Outcome::kOk) {
          return *why = "the upgrade box failed: " + (out.result.error.empty() ? std::string("it trapped or timed out") : out.result.error),
                 std::nullopt;
        }
        if (out.result.status != 200) {
          return *why = "the upgrade box answered " + std::to_string(out.result.status) + ": " + out.result.body.substr(0, 300),
                 std::nullopt;
        }
        std::string err;
        auto v = json::parse(out.result.body, &err);
        if (!v || !v->is_object()) return *why = "the upgrade box's answer isn't a JSON object, the row in the new shape", std::nullopt;
        return v->as_object();
      };
    }
    return out;
  };
  auto passed_file = [this](const Plan& p) {
    return config_.get()->storage / "upgrades" / (p.box->manifest.box + "." + p.box->version + ".dry_run.json");
  };
  auto box_prop = mcp::string_property("The upgrade box, as box_list names it: one whose manifest has routes.upgrade.");

  mcp_->add_tool(
      {"upgrade_dry_run",
       "Changing tool (it waits for approval when that's on), though it changes no data: runs an upgrade box "
       "(routes.upgrade, design/machine/041.SchemaEvolution.md § 7) against copies of every database on the host "
       "that holds the schema version it upgrades from: each row of each forced table passes through the box's rows "
       "handler, and is checked against the new version's shape (types, NOT NULL, CHECKs, unique indexes). The copies "
       "are then thrown away. Refused while a box (active or disabled) pinned to that version, or an older one, still "
       "uses a table the step rewrites. Returns {box, version, from, to, step, passed, databases: [{tenant, database, "
       "rows, failures: [{table, key, error}] (at most 20), error?}]}. upgrade_start needs a passed dry run of the "
       "same box version.",
       mcp::object_schema({{"box", box_prop}}, {"box"}),
       [this, plan, handlers, passed_file](const bj::object& args) {
         std::string err;
         auto p = plan(args, &err);
         if (!p) return ToolResult::fail(err);
         auto root = config_.get()->storage / "tmp" / "upgrades" / random_id();
         db::Databases copies(root);
         copies.set_id_source(next_id_);
         copies.set_schema_store(schemas_.get());
         bool passed = true;
         bj::array dbs;
         for (const auto& [tenant, database] : p->databases) {
           bj::object d{{"tenant", tenant}, {"database", database}};
           std::error_code ec;
           fs::create_directories(copies.file(tenant, database).parent_path(), ec);
           db::UpgradeReport report;
           std::string why;
           bool ok = copy_database(databases_->file(tenant, database), copies.file(tenant, database), &why);
           // A dry run applies whether or not a person approved yet: it is how they see what it does.
           if (ok) {
             ok = copies.upgrade(tenant, database, p->to, handlers(*p, tenant), true, &report, &why, false);
           }
           d["rows"] = report.rows;
           d["failures"] = report.failures;
           if (!ok) d["error"] = why;
           passed &= ok;
           dbs.push_back(std::move(d));
         }
         std::error_code ec;
         fs::remove_all(root, ec);
         auto file = passed_file(*p);
         if (passed) {
           fs::create_directories(file.parent_path(), ec);
           base::write_file_durable(file, json::dump(bj::object{{"at", base::format_utc_ms(base::now_unix_ms())},
                                                                {"by", current_operator()}}));
         } else {
           fs::remove(file, ec);
         }
         return ToolResult::ok(bj::object{{"box", p->box->manifest.box},
                                          {"version", p->box->version},
                                          {"from", p->from.name},
                                          {"to", p->to.name},
                                          {"step", p->step.to_json()},
                                          {"passed", passed},
                                          {"databases", std::move(dbs)}});
       }});

  mcp_->add_tool(
      {"upgrade_start",
       "Changing tool; goes live. Runs an upgrade box (routes.upgrade) for real on every database on the host that "
       "holds the version it upgrades from: for each one, the host takes a snapshot (db_snapshot), then, in one "
       "transaction while that database's writes wait, builds each forced table in the new shape, passes every row "
       "through the box's rows handler, checks it, and swaps the new table in, recording the new version. A "
       "database where any row fails rolls back whole and stays at the old version; the answer names the row and "
       "why, and the other databases go on. Refused until a person approved the new schema in the panel and a dry "
       "run of the same box version passed (upgrade_dry_run), and while a box pinned to the old version, or an "
       "older one, still uses a rewritten table. Boxes pinned to the new version serve a database once it has "
       "upgraded. Returns {box, version, from, to, databases: [{tenant, database, rows, upgraded, error?, "
       "failures?}]}.",
       mcp::object_schema({{"box", box_prop}}, {"box"}),
       [this, plan, handlers, passed_file](const bj::object& args) {
         std::string err;
         auto p = plan(args, &err);
         if (!p) return ToolResult::fail(err);
         if (schemas_->held(p->to.name)) {
           return ToolResult::fail("schema " + p->to.name + " waits for a person to approve it in the panel's Database "
                                   "view (041 § 6); upgrade_dry_run shows what it would do meanwhile");
         }
         if (!fs::exists(passed_file(*p))) {
           return ToolResult::fail("no dry run of " + p->box->manifest.box + " version " + p->box->version +
                                   " has passed: upgrade_dry_run first, and read what it reports");
         }
         bj::array dbs;
         for (const auto& [tenant, database] : p->databases) {
           db::UpgradeReport report;
           std::string why;
           bool ok = databases_->upgrade(tenant, database, p->to, handlers(*p, tenant), false, &report, &why);
           bj::object d{{"tenant", tenant}, {"database", database}, {"rows", report.rows}, {"upgraded", ok}};
           if (!ok) {
             d["error"] = why;
             d["failures"] = report.failures;
           }
           dbs.push_back(std::move(d));
         }
         return ToolResult::ok(bj::object{{"box", p->box->manifest.box},
                                          {"version", p->box->version},
                                          {"from", p->from.name},
                                          {"to", p->to.name},
                                          {"databases", std::move(dbs)}});
       }});
}

}  // namespace goober::host
