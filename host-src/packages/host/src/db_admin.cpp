// Operators and databases (design/machine/008.Databases.md § 11): snapshots of one database,
// restoring one to a new database, making a database from stored schemas before a box binds it,
// and what no box binds; and the drops only a person makes, in the panel.

#include <sqlite3.h>

#include <iostream>
#include <set>

#include "goober/backup/backup.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {


std::string text_arg(const bj::object& args, const char* key) {
  auto* v = args.if_contains(key);
  return v && v->is_string() ? std::string(v->as_string()) : "";
}

// The tables of a SQLite file and their row counts (host tables aside).
bj::array tables_of(const fs::path& file) {
  bj::array out;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return out;
  }
  std::vector<std::string> names;
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'goober\\_%' ESCAPE '\\' "
                         "AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name",
                         -1, &s, nullptr) == SQLITE_OK) {
    while (sqlite3_step(s) == SQLITE_ROW) names.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)));
  }
  sqlite3_finalize(s);
  for (const auto& t : names) {
    int64_t rows = 0;
    std::string sql = "SELECT count(*) FROM \"" + t + "\"";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) {
      rows = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    out.push_back(bj::object{{"table", t}, {"rows", rows}});
  }
  sqlite3_close(db);
  return out;
}

std::string quick_check(const fs::path& file) {
  sqlite3* db = nullptr;
  std::string result;
  if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
      result = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
  }
  sqlite3_close(db);
  return result;
}

}  // namespace

fs::path Host::snapshots_dir(const std::string& tenant) const {
  return config_.get()->storage / "snapshots" / tenant;
}

std::vector<Host::DbBinding> Host::db_bindings(const std::string& tenant, const std::string& database) {
  std::vector<DbBinding> out;
  auto cfg = config_.get();
  auto active = boxes_->active();
  for (const auto& name : boxes_->box_names()) {
    // Every version that may open it: the live one, a disabled box's (it can be enabled again),
    // and the beta version (053 § 3).
    struct Bound {
      std::shared_ptr<const manifest::Manifest> m;
      std::string version;
      bool disabled = false;
    };
    std::vector<Bound> versions;
    if (auto it = active->boxes.find(name); it != active->boxes.end()) {
      versions.push_back({std::shared_ptr<const manifest::Manifest>(it->second, &it->second->manifest), it->second->version});
    } else if (auto d = boxes_->disabled(name)) {
      auto text = boxes_->manifest_text(name, d->version);
      std::string err;
      auto parsed = text ? manifest::parse_manifest(*text, &err) : std::nullopt;
      if (parsed) versions.push_back({std::make_shared<const manifest::Manifest>(std::move(*parsed)), d->version, true});
    }
    if (auto it = active->beta.find(name); it != active->beta.end()) {
      versions.push_back({std::shared_ptr<const manifest::Manifest>(it->second, &it->second->manifest), it->second->version});
    }
    for (const auto& [m, version, disabled] : versions) {
      std::set<std::string> served;
      for (const auto& [t, domain] : tenants_of(*cfg, *m)) served.insert(t);
      for (const auto& d : m->data) {
        if (!d.applies_schema()) continue;
        // Whatever tenant a request comes for, the database its strategy opens: a fixed tenant's
        // beta for a beta request (DataStrategy::tenant_for).
        bool binds = !d.tenant.empty() && d.tenant == tenant && d.database_for(tenant) == database;
        for (const auto& r : served) binds |= d.tenant_for(r) == tenant && d.database_for(r) == database;
        if (!binds) continue;
        DbBinding b{name, version, d.name, d.schema, disabled, {}};
        for (const auto& [t, g] : d.tables) b.tables.insert(t);
        if (!d.table.empty()) b.tables.insert(d.table);
        if (auto schema = schemas_->get(d.schema)) {
          for (const auto& t : schema->tables) b.tables.insert(t.name);
        }
        out.push_back(std::move(b));
      }
    }
  }
  return out;
}

std::optional<bj::object> Host::take_snapshot(const std::string& tenant, const std::string& database,
                                              const std::string& note, const std::string& by, bool for_drop,
                                              std::string* error) {
  if (!databases_->exists(tenant, database)) {
    return *error = "tenant " + tenant + " has no database " + database, std::nullopt;
  }
  auto dir = snapshots_dir(tenant) / database;
  std::string near_limit;
  if (!for_drop) {
    int64_t bytes = 0;
    int count = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(snapshots_dir(tenant), ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (it->is_regular_file() && it->path().extension() == ".db") {
        bytes += int64_t(it->file_size());
        ++count;
      }
    }
    int64_t size = int64_t(fs::file_size(databases_->file(tenant, database), ec));
    // A tenant's snapshots may hold limits.snapshot-files files and limits.snapshot-bytes (045);
    // past either this refuses, and a person deletes old ones in the panel. The host never
    // deletes a snapshot itself. A drop's own snapshot is always taken.
    const auto& lim = config_.get()->disk;
    if (count >= lim.snapshot_files || bytes + size > lim.snapshot_bytes) {
      return *error = "tenant " + tenant + "'s snapshots hold " + std::to_string(count) + " files and " +
                      std::to_string(bytes >> 20) + " MiB; one more passes limits.snapshot-files (" +
                      std::to_string(lim.snapshot_files) + ") or limits.snapshot-bytes (" +
                      std::to_string(lim.snapshot_bytes >> 20) + " MiB). Ask a person to delete old ones in the "
                      "panel's Database view, or raise the limit in host.json",
             std::nullopt;
    }
    if (auto refused = free_space_refused(size, "a snapshot of " + database); !refused.empty()) {
      return *error = refused, std::nullopt;
    }
    // Near the limit (80%), the answer says so before the refusal comes.
    if ((count + 1) * 10 >= lim.snapshot_files * 8 || (bytes + size) * 10 >= lim.snapshot_bytes * 8) {
      near_limit = "tenant " + tenant + "'s snapshots are near their limit (" + std::to_string(count + 1) + " of " +
                   std::to_string(lim.snapshot_files) + " files, " + std::to_string((bytes + size) >> 20) + " of " +
                   std::to_string(lim.snapshot_bytes >> 20) + " MiB): ask a person to delete old ones in the panel";
    }
  }
  int64_t now = base::now_unix_ms();
  std::string stamp = backup::stamp(now);
  std::string id = stamp;
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (int n = 2; fs::exists(dir / (id + ".db"), ec); ++n) id = stamp + "-" + std::to_string(n);
  auto file = dir / (id + ".db");
  if (!copy_database(databases_->file(tenant, database), file, error)) return std::nullopt;
  auto sha = base::sha256_file_hex(file);
  if (!sha) return *error = "can't read the snapshot back", std::nullopt;
  bj::object record{{"snapshot", database + "/" + id},
                    {"tenant", tenant},
                    {"database", database},
                    {"at", base::format_utc_ms(now)},
                    {"by", by},
                    {"note", note.substr(0, 500)},
                    {"sha256", *sha},
                    {"bytes", int64_t(fs::file_size(file, ec))},
                    {"tables", tables_of(file)}};
  if (for_drop) record["before_drop"] = true;
  if (!base::write_file_durable(dir / (id + ".json"), json::pretty(record))) {
    return *error = "can't write the snapshot's record", std::nullopt;
  }
  if (!near_limit.empty()) record["warning"] = near_limit;
  return record;
}

std::vector<bj::object> Host::list_snapshots(const std::string& tenant, const std::string& database) {
  std::vector<bj::object> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(snapshots_dir(tenant), ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file() || it->path().extension() != ".json") continue;
    auto text = base::read_file(it->path());
    std::string err;
    auto v = text ? json::parse(*text, &err) : std::nullopt;
    if (!v || !v->is_object()) continue;
    if (!database.empty() && text_arg(v->as_object(), "database") != database) continue;
    out.push_back(v->as_object());
  }
  std::sort(out.begin(), out.end(), [](const bj::object& a, const bj::object& b) {
    return text_arg(a, "snapshot") > text_arg(b, "snapshot");
  });
  return out;
}

// A snapshot id is DATABASE/STAMP: its two files under the tenant's snapshots, or nullopt.
std::optional<std::pair<fs::path, bj::object>> Host::find_snapshot(const std::string& tenant, const std::string& id) {
  auto slash = id.find('/');
  if (slash == std::string::npos) return std::nullopt;
  std::string database = id.substr(0, slash), stamp = id.substr(slash + 1);
  if (!db::is_valid_sql_name(database) || stamp.empty() || stamp.find_first_not_of("0123456789TZ-") != std::string::npos) {
    return std::nullopt;
  }
  auto dir = snapshots_dir(tenant) / database;
  auto text = base::read_file(dir / (stamp + ".json"));
  std::string err;
  auto v = text ? json::parse(*text, &err) : std::nullopt;
  if (!v || !v->is_object() || !fs::exists(dir / (stamp + ".db"))) return std::nullopt;
  return std::make_pair(dir / (stamp + ".db"), v->as_object());
}

void Host::register_db_admin_tools() {
  using mcp::object_schema;
  using mcp::string_property;
  auto known = [this](const std::string& tenant) {
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == tenant) return true;
    }
    return databases_->databases(tenant).size() > 0;
  };
  auto tenant_prop = string_property("A tenant name, like example.");
  auto db_prop = string_property("A database name, like orders.");

  mcp_->add_tool(
      {"db_snapshot",
       "Changing tool. Copies one tenant database as it is now (SQLite's online backup, safe while boxes write) into the host's "
       "snapshots, and answers its id (DATABASE/STAMP), SHA-256, size, and tables with row counts. Before a migration "
       "or a drop. Snapshots are kept until a person deletes them in the panel, and the nightly backup carries them. A "
       "tenant's snapshots may hold limits.snapshot-files files and limits.snapshot-bytes (100 and 2 GiB unless host.json says otherwise); near that the answer warns, past it this refuses. db_snapshots lists them; "
       "db_restore_to_new makes a new database from one.",
       object_schema({{"tenant", tenant_prop}, {"database", db_prop}, {"note", string_property("Why, for whoever reads the list.")}},
                     {"tenant", "database"}),
       [this, known](const bj::object& args) {
         std::string tenant = text_arg(args, "tenant"), database = text_arg(args, "database");
         if (!known(tenant)) return ToolResult::fail("no tenant named " + tenant);
         std::string err;
         auto r = take_snapshot(tenant, database, text_arg(args, "note"), current_operator(), false, &err);
         if (!r) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*r));
       }});
  mcp_->add_tool({"db_snapshots",
                  "Read tool. A tenant's database snapshots, newest first: id (DATABASE/STAMP), when, who, the note, SHA-256, size, "
                  "tables with row counts, and before_drop for the ones the host took before a person dropped something.",
                  object_schema({{"tenant", tenant_prop}, {"database", string_property("Only this database's.")}}, {"tenant"}),
                  [this](const bj::object& args) {
                    bj::array out;
                    for (auto& s : list_snapshots(text_arg(args, "tenant"), text_arg(args, "database"))) out.push_back(std::move(s));
                    return ToolResult::ok(bj::object{{"snapshots", std::move(out)}});
                  }});
  mcp_->add_tool(
      {"db_restore_to_new",
       "Changing tool. Makes a NEW database from a snapshot: the snapshot's tables, rows, and applied schemas, "
       "checked against its SHA-256 and by SQLite's quick_check. Never writes over a database: the name must be "
       "free. Boxes reach it only by naming it in a strategy. Returns database, from, schemas, and tables.",
       object_schema({{"tenant", tenant_prop},
                      {"snapshot", string_property("The snapshot's id, DATABASE/STAMP, from db_snapshots.")},
                      {"database", string_property("The new database's name, unused in this tenant.")}},
                     {"tenant", "snapshot", "database"}),
       [this](const bj::object& args) {
         std::string tenant = text_arg(args, "tenant"), id = text_arg(args, "snapshot"), database = text_arg(args, "database");
         if (!db::is_valid_sql_name(database)) return ToolResult::fail("database must match [a-z][a-z0-9_]*");
         if (databases_->exists(tenant, database)) {
           return ToolResult::fail("tenant " + tenant + " already has a database " + database + ": restore into a new name");
         }
         auto snap = find_snapshot(tenant, id);
         if (!snap) return ToolResult::fail("no snapshot " + id + " for tenant " + tenant + " (see db_snapshots)");
         auto sha = base::sha256_file_hex(snap->first);
         if (!sha || *sha != text_arg(snap->second, "sha256")) {
           return ToolResult::fail("snapshot " + id + " doesn't match its recorded SHA-256; it wasn't used");
         }
         auto target = databases_->file(tenant, database);
         std::error_code ec;
         fs::create_directories(target.parent_path(), ec);
         std::string err;
         if (!copy_database(snap->first, target, &err)) return ToolResult::fail(err);
         if (auto check = quick_check(target); check != "ok") {
           fs::remove(target, ec);
           return ToolResult::fail("the restored copy fails SQLite's check (" + check + "); removed");
         }
         databases_->forget(tenant, database);
         auto d = databases_->describe(tenant, database);
         return ToolResult::ok(bj::object{{"tenant", tenant}, {"database", database}, {"from", id},
                                          {"schemas", d.at("schemas")}, {"tables", tables_of(target)}});
       }});
  mcp_->add_tool(
      {"db_create",
       "Changing tool. Makes a new, empty tenant database and applies stored schemas to it, before any box binds "
       "it, so it can be checked empty (db_describe). The schemas must be stored (schema_put) and fit together, as "
       "boxes sharing a database must. Refused when the database exists or a schema is retired. Returns what "
       "db_describe returns.",
       object_schema({{"tenant", tenant_prop},
                      {"database", string_property("The new database's name.")},
                      {"schemas", mcp::any_property("The stored schemas to apply, in order: [\"main1\", ...].")}},
                     {"tenant", "database", "schemas"}),
       [this, known](const bj::object& args) {
         std::string tenant = text_arg(args, "tenant"), database = text_arg(args, "database");
         if (!known(tenant)) return ToolResult::fail("no tenant named " + tenant);
         if (!db::is_valid_sql_name(database)) return ToolResult::fail("database must match [a-z][a-z0-9_]*");
         if (databases_->exists(tenant, database)) return ToolResult::fail("tenant " + tenant + " already has a database " + database);
         auto* list = args.if_contains("schemas");
         if (!list || !list->is_array() || list->as_array().empty()) return ToolResult::fail("schemas is a list of stored schema names");
         std::vector<db::Schema> found;
         for (const auto& n : list->as_array()) {
           if (!n.is_string()) return ToolResult::fail("schemas is a list of stored schema names");
           std::string name(n.as_string());
           if (schemas_->retired(name)) return ToolResult::fail("schema " + name + " is retired");
           auto s = schemas_->get(name);
           if (!s) return ToolResult::fail("schema " + name + " isn't stored; schema_put it first");
           found.push_back(std::move(*s));
         }
         std::vector<const db::Schema*> ptrs;
         for (const auto& s : found) ptrs.push_back(&s);
         std::string err;
         if (!db::Databases::compatible(ptrs, &err, schemas_.get())) return ToolResult::fail(err);
         for (const auto& s : found) {
           if (!databases_->ensure_schema(tenant, database, s, &err)) {
             std::error_code ec;
             databases_->drop_database(tenant, database, &err);
             return ToolResult::fail("applying " + s.name + ": " + err);
           }
         }
         return ToolResult::ok(databases_->describe(tenant, database));
       }});
  mcp_->add_tool(
      {"db_unbound",
       "Read tool. A tenant's databases and tables that no box binds (active or disabled), and stored schemas no box names: what a "
       "person might drop or retire in the panel; and held_schemas: stored schemas whose step drops or rewrites data, "
       "waiting for a person to approve them in the panel, each with its step and what it would do to each database "
       "(design/machine/041.SchemaEvolution.md § 6). Dropping and approving are a person's acts: an agent "
       "asks with db_drop (or box_drop for a box), which waits for a host:owner in the panel's Cleanup view.",
       object_schema({{"tenant", tenant_prop}}, {"tenant"}), [this](const bj::object& args) {
         std::string tenant = text_arg(args, "tenant");
         bj::array dbs;
         for (const auto& database : databases_->databases(tenant)) {
           auto binds = db_bindings(tenant, database);
           std::set<std::string> bound_tables;
           bj::array by;
           for (const auto& b : binds) {
             bound_tables.insert(b.tables.begin(), b.tables.end());
             by.push_back(bj::object{{"box", b.box}, {"strategy", b.strategy}, {"schema", b.schema}, {"disabled", b.disabled}});
           }
           bj::array unbound;
           for (const auto& t : tables_of(databases_->file(tenant, database))) {
             if (!bound_tables.count(std::string(t.at("table").as_string()))) unbound.push_back(t);
           }
           dbs.push_back(bj::object{{"database", database},
                                    {"bound", !binds.empty()},
                                    {"bound_by", std::move(by)},
                                    {"unbound_tables", binds.empty() ? bj::array{} : std::move(unbound)}});
         }
         // Schemas no box names, active or disabled (host-wide).
         std::set<std::string> named;
         auto active = boxes_->active();
         for (const auto& name : boxes_->box_names()) {
           std::optional<manifest::Manifest> m;
           if (auto it = active->boxes.find(name); it != active->boxes.end()) {
             m = it->second->manifest;
           } else if (auto d = boxes_->disabled(name)) {
             std::string err;
             if (auto text = boxes_->manifest_text(name, d->version)) m = manifest::parse_manifest(*text, &err);
           }
           if (!m) continue;
           for (const auto& d : m->data) {
             if (!d.schema.empty()) named.insert(d.schema);
           }
         }
         bj::array schemas, held;
         for (const auto& s : schemas_->list()) {
           if (!named.count(s) && !schemas_->retired(s)) schemas.push_back(bj::value(s));
           // Waiting for a person (041 § 6): the step they would approve.
           if (schema_held(s)) {
             if (auto schema = schemas_->get(s)) {
               auto report = schema_step_report(*schema);
               held.push_back(bj::object{{"schema", s}, {"step", report["step"]}, {"databases", report["databases"]}});
             }
           }
         }
         return ToolResult::ok(bj::object{{"tenant", tenant},
                                          {"databases", std::move(dbs)},
                                          {"unnamed_schemas", std::move(schemas)},
                                          {"held_schemas", std::move(held)}});
       }});
}

// The panel's drops (008 § 11): a person's act alone. host:owner only; the person types the
// name of what goes; the host refuses while any box binds it, takes a snapshot first, and says
// where it is.
std::pair<int, bj::object> Host::panel_drop(const SignedIn& who, const bj::object& args, const std::string& request_id) {
  if (std::find(who.roles.begin(), who.roles.end(), "host:owner") == who.roles.end()) {
    return {403, bj::object{{"error", "only host:owner may drop a database, a table, or a snapshot, retire a schema, or approve one"}}};
  }
  std::string kind = text_arg(args, "kind"), tenant = text_arg(args, "tenant"), database = text_arg(args, "database"),
              table = text_arg(args, "table"), schema = text_arg(args, "schema"), snapshot = text_arg(args, "snapshot"),
              confirm = text_arg(args, "confirm"), why = text_arg(args, "why");
  auto scope = operator_scope("panel:" + who.user.email);
  std::string by = "panel:" + who.user.email;
  auto record = [&](const std::string& what, bj::object detail) {
    if (!changes_) return;
    try {
      detail["why"] = why.substr(0, 500);
      changes_->record_action(operations::Action{base::now_unix_ms(), by, what, request_id, std::move(detail), false});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  };
  std::string err;
  // Approving a schema whose step drops or rewrites data (041 § 6): host-wide, like retiring one.
  if (kind == "schema_approve") {
    if (!scope.all) return {403, bj::object{{"error", "schemas are host-wide: approving one needs a person focused on every tenant"}}};
    if (!schemas_->get(schema)) return {400, bj::object{{"error", "no schema named " + schema}}};
    if (confirm != schema) return {400, bj::object{{"error", "type the schema's name to approve it"}}};
    bj::object rec{{"by", by}, {"at", base::format_utc_ms(base::now_unix_ms())}, {"why", why.substr(0, 500)}};
    if (!schemas_->approve(schema, json::pretty(rec), &err)) return {400, bj::object{{"error", err}}};
    record("schema_approve", bj::object{{"schema", schema}});
    return {200, bj::object{{"approved", schema}}};
  }
  if (kind == "schema") {
    if (!scope.all) return {403, bj::object{{"error", "schemas are host-wide: retiring one needs a person focused on every tenant"}}};
    if (!schemas_->get(schema)) return {400, bj::object{{"error", "no schema named " + schema}}};
    if (confirm != schema) return {400, bj::object{{"error", "type the schema's name to retire it"}}};
    std::vector<std::string> users;
    auto active = boxes_->active();
    for (const auto& name : boxes_->box_names()) {
      std::optional<manifest::Manifest> m;
      if (auto it = active->boxes.find(name); it != active->boxes.end()) {
        m = it->second->manifest;
      } else if (auto d = boxes_->disabled(name)) {
        if (auto text = boxes_->manifest_text(name, d->version)) m = manifest::parse_manifest(*text, &err);
      }
      if (!m) continue;
      for (const auto& d : m->data) {
        if (d.schema == schema) users.push_back(name + "." + d.name);
      }
    }
    if (!users.empty()) {
      std::string list;
      for (const auto& u : users) list += (list.empty() ? "" : ", ") + u;
      return {409, bj::object{{"error", "schema " + schema + " is named by " + list + "; retire it once no box does"}}};
    }
    bj::object rec{{"by", by}, {"at", base::format_utc_ms(base::now_unix_ms())}, {"why", why.substr(0, 500)}};
    if (!schemas_->retire(schema, json::pretty(rec), &err)) return {400, bj::object{{"error", err}}};
    record("schema_retire", bj::object{{"schema", schema}});
    return {200, bj::object{{"retired", schema}}};
  }
  if (!scope.all && !scope.tenants.count(tenant)) return {403, bj::object{{"error", "outside your scope"}}};
  if (kind == "snapshot") {
    auto snap = find_snapshot(tenant, snapshot);
    if (!snap) return {400, bj::object{{"error", "no snapshot " + snapshot}}};
    if (confirm != snapshot) return {400, bj::object{{"error", "type the snapshot's id to delete it"}}};
    std::error_code ec;
    fs::remove(snap->first, ec);
    fs::path json_file = snap->first;
    json_file.replace_extension(".json");
    fs::remove(json_file, ec);
    record("db_snapshot_delete", bj::object{{"tenant", tenant}, {"snapshot", snapshot}});
    return {200, bj::object{{"deleted", snapshot}}};
  }
  if (kind == "box") {
    if (!scope.all) return {403, bj::object{{"error", "dropping a box needs a person focused on every tenant"}}};
    if (confirm != text_arg(args, "box")) return {400, bj::object{{"error", "type the box's name to drop it"}}};
    return drop_box(confirm, by, why, "", request_id);
  }
  if (kind != "database" && kind != "table") {
    return {400, bj::object{{"error", "kind is box, database, table, schema, snapshot, or schema_approve"}}};
  }
  if (!databases_->exists(tenant, database)) return {400, bj::object{{"error", "tenant " + tenant + " has no database " + database}}};
  std::string target = kind == "table" ? database + "." + table : database;
  if (confirm != target) return {400, bj::object{{"error", "type " + target + " to drop it"}}};
  return drop_database(tenant, database, kind == "table" ? table : "", by, why, "", request_id);
}

bj::object Host::db_drop_check(const std::string& tenant, const std::string& database, const std::string& table) {
  std::string target = table.empty() ? database : database + "." + table;
  bj::object out{{"tenant", tenant}, {"database", database}, {"target", target}};
  if (!base::is_valid_name(tenant) || !databases_->exists(tenant, database)) {
    out["exists"] = false;
    out["blocked"] = bj::array{bj::value("tenant " + tenant + " has no database " + database)};
    return out;
  }
  out["exists"] = true;
  auto file = databases_->file(tenant, database);
  std::error_code ec;
  int64_t bytes = 0;
  for (const char* ext : {"", "-wal"}) {
    auto n = fs::file_size(file.string() + ext, ec);
    if (!ec) bytes += int64_t(n);
  }
  out["bytes"] = bytes;
  auto tables = tables_of(file);
  bool found = table.empty();
  int64_t rows = 0;
  for (const auto& t : tables) {
    if (!table.empty() && t.at("table").as_string() == table) {
      found = true;
      rows = t.at("rows").as_int64();
    } else if (table.empty()) {
      rows += t.at("rows").as_int64();
    }
  }
  out["tables"] = table.empty() ? bj::value(tables) : bj::value(nullptr);
  out["rows"] = rows;
  bj::array blocked, by;
  if (!found) blocked.push_back(bj::value("database " + database + " has no table " + table));
  for (const auto& b : db_bindings(tenant, database)) {
    if (!table.empty() && !b.tables.count(table)) continue;
    by.push_back(bj::object{{"box", b.box}, {"strategy", b.strategy}, {"disabled", b.disabled}});
    blocked.push_back(bj::value("bound by " + b.box + "." + b.strategy + (b.disabled ? " (disabled)" : "") +
                                ": drop the box first (box_drop), or deploy a version that no longer binds it"));
  }
  out["bound_by"] = std::move(by);
  out["blocked"] = std::move(blocked);
  out["takes"] = "a snapshot first (kept until a person deletes it), then " + std::string(table.empty() ? "the database" : "the table") +
                 " and its rows";
  return out;
}

std::pair<int, bj::object> Host::drop_database(const std::string& tenant, const std::string& database, const std::string& table,
                                               const std::string& by, const std::string& why, const std::string& approved_by,
                                               const std::string& request_id) {
  auto check = db_drop_check(tenant, database, table);
  std::string target = std::string(check.at("target").as_string());
  if (!check.at("blocked").as_array().empty()) {
    std::string list;
    for (const auto& b : check.at("blocked").as_array()) list += (list.empty() ? "" : "; ") + std::string(b.as_string());
    return {409, bj::object{{"error", target + " can't be dropped now: " + list}, {"check", std::move(check)}}};
  }
  std::string err;
  auto snap = take_snapshot(tenant, database, "before dropping " + target + (why.empty() ? "" : ": " + why), by, true, &err);
  if (!snap) return {500, bj::object{{"error", "the snapshot before the drop failed, so nothing was dropped: " + err}}};
  bool ok = !table.empty() ? databases_->drop_table(tenant, database, table, &err) : databases_->drop_database(tenant, database, &err);
  if (!ok) return {400, bj::object{{"error", err}, {"snapshot", snap->at("snapshot")}}};
  bj::object detail{{"tenant", tenant}, {"target", target}, {"snapshot", snap->at("snapshot")}, {"why", why.substr(0, 500)},
                    {"rows", check.at("rows")}};
  if (!approved_by.empty()) detail["approved_by"] = approved_by;
  if (changes_) {
    try {
      changes_->record_action(operations::Action{base::now_unix_ms(), by, !table.empty() ? "table_drop" : "db_drop", request_id, detail, false});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  }
  audit(!table.empty() ? "drop.table" : "drop.database", by, tenant + "/" + target, "ok", detail);
  return {200, bj::object{{"dropped", target}, {"tenant", tenant}, {"snapshot", snap->at("snapshot")}}};
}

}  // namespace goober::host
