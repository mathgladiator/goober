// Schemas and tenant databases for operators (design/machine/008.Databases.md §7), and
// the upload check for a manifest's data section.

#include <set>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

}  // namespace

bool Host::check_data(const manifest::Manifest& m, std::string* error) const {
  std::set<std::string> tenants;
  for (const auto& [d, e] : config_.get()->domains) tenants.insert(e.tenant);
  for (const auto& t : databases_->tenants()) tenants.insert(t);
  for (const auto& st : m.data) {
    if (!st.applies_schema()) continue;
    std::string where = "data.sqlite3." + st.name;
    auto schema = schemas_->get(st.schema);
    if (!schema) return *error = where + ": schema " + st.schema + " isn't stored on this host; store it with schema_put first", false;
    if (st.pattern == "by_timer") {
      // The scheduler reads these columns as they are (045), so they must exist.
      const auto* t = schema->table(st.table);
      if (!t) return *error = where + ": schema " + st.schema + " has no table " + st.table, false;
      std::vector<std::string> cols = {st.timezone_field, st.active_field};
      for (const auto& timer : m.timers) {
        if (timer.source == st.name && !timer.at_field.empty()) cols.push_back(timer.at_field);
      }
      for (const auto& c : cols) {
        if (!c.empty() && !t->column(c)) return *error = where + ": schema " + st.schema + " has no column " + st.table + "." + c, false;
      }
    }
    for (const auto& [table, grant] : st.tables) {
      const auto* t = schema->table(table);
      if (!t) return *error = where + ": schema " + st.schema + " has no table " + table, false;
      for (const auto* cols : {&grant.read, &grant.update}) {
        for (const auto& c : *cols) {
          if (!t->column(c)) return *error = where + ": schema " + st.schema + " has no column " + table + "." + c, false;
        }
      }
    }
    for (const auto& tenant : tenants) {
      if (!databases_->check(tenant, st.database_for(tenant), *schema, error)) {
        *error = where + ": " + *error;
        return false;
      }
    }
  }
  return true;
}

void Host::register_data_tools() {
  using mcp::any_property;
  mcp_->add_tool({"timers_list",
                  "The timers the host has planned (045): each timer, tenant, and row, with when it is next due.",
                  mcp::object_schema({}, {}),
                  [this](const bj::object&) { return ToolResult::ok(timers_due()); }});
  mcp_->add_tool({"timer_run",
                  "Fires a timer now for one tenant, once per active row of its source, as its schedule would. For "
                  "trying a timer without waiting for its time.",
                  mcp::object_schema({{"timer", mcp::string_property("The timer's route name.")},
                                      {"tenant", mcp::string_property("A tenant, like playground.")}},
                                     {"timer", "tenant"}),
                  [this](const bj::object& args) {
                    auto timer = arg(args, "timer");
                    auto tenant = arg(args, "tenant");
                    if (!timer || !tenant) return ToolResult::fail("timer and tenant are required");
                    std::string err;
                    auto fired = run_timer_now(*timer, *tenant, &err);
                    if (!fired) return ToolResult::fail(err);
                    return ToolResult::ok(bj::object{{"fired", *fired}});
                  }});
  using mcp::boolean_property;
  using mcp::integer_property;
  using mcp::object_schema;
  using mcp::string_property;

  auto tenant_prop = string_property("A tenant name, like playground.");
  auto db_prop = string_property("A database name, like guestbook.");

  mcp_->add_tool(
      {"schema_put",
       "Stores a database schema: {\"schema\": \"name1\", \"tables\": {\"t\": {\"columns\": {\"c\": {\"type\": "
       "\"string\"}}, \"indexes\": {...}}}}. Types are int32, int64, double, float, boolean, and string; columns "
       "take nullable, default, max-length, and primary-key. A schema is a version: its name can't be reused with "
       "different content, and databases only ever grow by the schemas applied to them.",
       object_schema({{"schema", any_property("The schema, as a JSON object or JSON text.")}}, {"schema"}),
       [this](const bj::object& args) {
         auto it = args.find("schema");
         if (it == args.end()) return ToolResult::fail("schema is required");
         std::string text = it->value().is_string() ? std::string(it->value().get_string()) : json::dump(it->value());
         std::string err;
         auto s = schemas_->put(text, &err);
         if (!s) return ToolResult::fail(err);
         bj::array tables;
         for (const auto& t : s->tables) tables.push_back(bj::value(t.name));
         return ToolResult::ok(bj::object{{"schema", s->name}, {"tables", std::move(tables)}});
       }});

  mcp_->add_tool({"schema_get", "Returns a stored schema.", object_schema({{"name", string_property("The schema's name.")}}, {"name"}),
                  [this](const bj::object& args) {
                    auto name = arg(args, "name");
                    if (!name) return ToolResult::fail("name is required");
                    auto s = schemas_->get(*name);
                    if (!s) return ToolResult::fail("no schema named " + *name);
                    std::string err;
                    return ToolResult::ok(*json::parse(s->text, &err));
                  }});

  mcp_->add_tool({"schema_list", "Lists the stored schemas.", object_schema({}, {}), [this](const bj::object&) {
                    bj::array out;
                    for (const auto& n : schemas_->list()) out.push_back(bj::value(n));
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool(
      {"db_list",
       "Lists every tenant's databases: the boxes and strategies that use each one, whether it exists yet (they are "
       "created on first use), its size, and its tables with row counts.",
       object_schema({{"tenant", string_property("Only this tenant.")}}, {}), [this](const bj::object& args) {
         std::set<std::string> tenants;
         for (const auto& [d, e] : config_.get()->domains) tenants.insert(e.tenant);
         for (const auto& t : databases_->tenants()) tenants.insert(t);
         if (auto only = arg(args, "tenant")) tenants = {*only};
         auto active = boxes_->active();
         bj::array out;
         for (const auto& tenant : tenants) {
           std::map<std::string, bj::array> users;
           for (const auto& [name, box] : active->boxes) {
             for (const auto& st : box->manifest.data) {
               if (!st.applies_schema()) continue;
               users[st.database_for(tenant)].push_back(
                   bj::object{{"box", name}, {"strategy", st.name}, {"schema", st.schema}});
             }
           }
           for (const auto& d : databases_->databases(tenant)) users[d];
           bj::array dbs;
           for (auto& [database, used_by] : users) {
             bj::object e{{"database", database}, {"used_by", std::move(used_by)}};
             auto d = databases_->describe(tenant, database);
             e["exists"] = d.at("exists");
             if (d.at("exists").as_bool()) {
               e["bytes"] = d.at("bytes");
               bj::array tables;
               for (const auto& t : d.at("tables").as_array()) {
                 tables.push_back(bj::object{{"table", t.at("table")}, {"rows", t.at("rows")}});
               }
               e["tables"] = std::move(tables);
             }
             dbs.push_back(std::move(e));
           }
           out.push_back(bj::object{{"tenant", tenant}, {"databases", std::move(dbs)}});
         }
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool({"db_describe", "Describes one tenant database: tables, columns, indexes, row counts, and the schemas applied.",
                  object_schema({{"tenant", tenant_prop}, {"database", db_prop}}, {"tenant", "database"}),
                  [this](const bj::object& args) {
                    auto tenant = arg(args, "tenant");
                    auto database = arg(args, "database");
                    if (!tenant || !database) return ToolResult::fail("tenant and database are required");
                    return ToolResult::ok(databases_->describe(*tenant, *database));
                  }});

  auto params_prop = any_property("Parameters: an array for ? placeholders, or an object for :name ones.");
  mcp_->add_tool(
      {"db_query",
       "Runs one read-only SQL statement on a tenant's database and returns columns and rows.",
       object_schema({{"tenant", tenant_prop},
                      {"database", db_prop},
                      {"sql", string_property("One SELECT (or other read-only) statement.")},
                      {"params", params_prop},
                      {"limit", integer_property("The most rows to return: default 100, at most 1000.")}},
                     {"tenant", "database", "sql"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto database = arg(args, "database");
         auto sql = arg(args, "sql");
         if (!tenant || !database || !sql) return ToolResult::fail("tenant, database, and sql are required");
         auto it = args.find("params");
         bj::value params = it == args.end() ? bj::value(nullptr) : it->value();
         int64_t limit = 100;
         if (auto l = args.find("limit"); l != args.end() && l->value().is_int64()) limit = l->value().get_int64();
         limit = std::clamp<int64_t>(limit, 1, 1000);
         std::string err;
         auto r = databases_->operate(*tenant, *database, *sql, params, limit, false, &err);
         if (!r) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*r));
       }});

  mcp_->add_tool(
      {"db_exec",
       "Runs one INSERT, UPDATE, or DELETE on a tenant's database and returns the rows changed. Schema changes and "
       "the host's goober_ tables are refused. Every call is logged to logs/db.log.",
       object_schema({{"tenant", tenant_prop},
                      {"database", db_prop},
                      {"sql", string_property("One INSERT, UPDATE, or DELETE.")},
                      {"params", params_prop}},
                     {"tenant", "database", "sql"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant");
         auto database = arg(args, "database");
         auto sql = arg(args, "sql");
         if (!tenant || !database || !sql) return ToolResult::fail("tenant, database, and sql are required");
         auto it = args.find("params");
         bj::value params = it == args.end() ? bj::value(nullptr) : it->value();
         std::string err;
         auto r = databases_->operate(*tenant, *database, *sql, params, 1000, true, &err);
         base::append_line(config_.get()->storage / "logs" / "db.log",
                           json::dump(bj::object{{"at", base::format_utc_ms(base::now_unix_ms())},
                                                 {"operator", current_operator()},
                                                 {"tenant", *tenant},
                                                 {"database", *database},
                                                 {"sql", *sql},
                                                 {"params", params},
                                                 {"changes", r ? r->at("changes") : bj::value(nullptr)},
                                                 {"error", r ? "" : err}}));
         if (!r) return ToolResult::fail(err);
         return ToolResult::ok(bj::object{{"changes", r->at("changes")}, {"last_insert_id", r->at("last_insert_id")}});
       }});
}

}  // namespace goober::host
