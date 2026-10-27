// Schemas and tenant databases for operators (design/machine/008.Databases.md §7), and
// the upload check for a manifest's data section.

#include <deque>
#include <functional>
#include <set>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "box_guide.h"
#include "keys.h"
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

// Both sides of every internal call agree, and no chain of calls comes back around (039).
bool Host::check_calls(const manifest::Manifest& m, std::string* error) const {
  auto active = boxes_->active();
  // Every box's manifest as it would be with this version in place.
  std::map<std::string, const manifest::Manifest*> boxes;
  for (const auto& [name, box] : active->boxes) boxes[name] = &box->manifest;
  boxes[m.box] = &m;
  auto route_for = [&](const manifest::Manifest& callee, const std::string& name) -> const manifest::HttpRoute* {
    for (const auto& r : callee.internal) {
      if (r.name == name) return &r;
    }
    return nullptr;
  };
  for (const auto& [name, man] : boxes) {
    for (const auto& svc : man->external) {
      if (svc.box.empty()) continue;
      std::string where = (name == m.box ? "" : "box " + name + ": ") + "external." + svc.name;
      if (svc.box == name) return *error = where + " calls its own box", false;
      auto callee = boxes.find(svc.box);
      if (callee == boxes.end()) return *error = where + " calls box " + svc.box + ", which isn't deployed", false;
      for (const auto& [method, _] : svc.methods) {
        const auto* r = route_for(*callee->second, method);
        if (!r) return *error = where + " calls " + svc.box + "." + method + ", which isn't an internal route of it", false;
        if (std::find(r->callers.begin(), r->callers.end(), name) == r->callers.end()) {
          return *error = where + " calls " + svc.box + "." + method + ", whose callers don't list " + name, false;
        }
      }
    }
  }
  // No cycles: walk the call graph from every box.
  std::map<std::string, int> state;  // 1 visiting, 2 done
  std::function<bool(const std::string&, std::string)> visit = [&](const std::string& box, std::string path) {
    if (state[box] == 1) return *error = "internal calls would loop: " + path + " -> " + box, false;
    if (state[box] == 2) return true;
    state[box] = 1;
    if (auto it = boxes.find(box); it != boxes.end()) {
      for (const auto& svc : it->second->external) {
        if (!svc.box.empty() && !visit(svc.box, path.empty() ? box : path + " -> " + box)) return false;
      }
    }
    state[box] = 2;
    return true;
  };
  for (const auto& [name, _] : boxes) {
    if (!visit(name, "")) return false;
  }
  return true;
}

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
    // Boxes that name the same database share it, so their schemas must fit together.
    std::vector<const db::Schema*> together;
    std::deque<db::Schema> held;  // stable addresses for `together`
    auto active = boxes_->active();
    for (const auto& [name, box] : active->boxes) {
      if (name == m.box) continue;
      for (const auto& other : box->manifest.data) {
        if (!other.applies_schema() || other.database != st.database) continue;
        if (!other.tenant.empty() && !st.tenant.empty() && other.tenant != st.tenant) continue;
        if (auto s = schemas_->get(other.schema)) together.push_back(&held.emplace_back(std::move(*s)));
      }
    }
    if (!together.empty()) {
      together.push_back(&*schema);
      if (!db::Databases::compatible(together, error)) {
        *error = where + ": database " + st.database + " is shared with other boxes, and " + *error;
        return false;
      }
    }
    for (const auto& tenant : st.tenant.empty() ? tenants : std::set<std::string>{st.tenant}) {
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
  mcp_->add_tool({"box_guide",
                  "Read this first when building a product: how boxes, manifests, routes, MCP tools, timers, "
                  "databases, and types work on goober, and how to deploy them.",
                  mcp::object_schema({}, {}), [](const bj::object&) { return ToolResult::ok(bj::value(kBoxGuide)); }});
  // Keys for external services (015 §5): write-only.
  auto scope_props = [](std::initializer_list<std::pair<std::string, bj::value>> extra) {
    bj::object props{{"scope", mcp::enum_property("system, for every tenant; or tenant, for one.", {"system", "tenant"})},
                     {"tenant", mcp::string_property("The tenant, when scope is tenant.")},
                     {"name", mcp::string_property("The key's name, like stripe or openai.")}};
    for (const auto& [k, v] : extra) props[k] = v;
    return bj::object{{"type", "object"}, {"properties", props}, {"required", bj::array{"scope", "name"}}};
  };
  mcp_->add_tool({"key_set",
                  "Sets a credential that boxes name in external.$service.credentials, like keys/system/openai or "
                  "keys/tenant/{!tenant}/stripe. Write-only: no tool ever reads a value back, and boxes never see it.",
                  scope_props({{"value", mcp::string_property("The secret, 1 to 8192 printable bytes.")}}),
                  [this](const bj::object& args) {
                    std::string err;
                    if (!keys_->set(arg(args, "scope").value_or(""), arg(args, "tenant").value_or(""),
                                    arg(args, "name").value_or(""), arg(args, "value").value_or(""), base::now_unix_ms(),
                                    &err)) {
                      return ToolResult::fail(err);
                    }
                    return ToolResult::ok(bj::object{{"set", true}});
                  }});
  mcp_->add_tool({"key_list", "The keys that are set: names, when, and a fingerprint, never values.",
                  mcp::object_schema({}, {}), [this](const bj::object&) { return ToolResult::ok(keys_->list()); }});
  mcp_->add_tool({"key_delete", "Removes a key. Calls that name it fail until it is set again.", scope_props({}),
                  [this](const bj::object& args) {
                    if (!keys_->remove(arg(args, "scope").value_or(""), arg(args, "tenant").value_or(""),
                                       arg(args, "name").value_or(""))) {
                      return ToolResult::fail("no such key");
                    }
                    return ToolResult::ok(bj::object{{"removed", true}});
                  }});
  // The outbox's dead letters (035): what couldn't be delivered, and what to do about it.
  mcp_->add_tool({"outbox_dead_letters",
                  "Effectful calls the outbox gave up on: which box, service, and request, how many attempts, and the "
                  "last status or error.",
                  mcp::object_schema({}, {}), [this](const bj::object&) { return ToolResult::ok(outbox_list("dead")); }});
  auto outbox_ref = mcp::object_schema({{"tenant", mcp::string_property("The tenant.")},
                                        {"database", mcp::string_property("The database, as outbox_dead_letters shows it.")},
                                        {"id", mcp::integer_property("The element's id.")}},
                                       {"tenant", "database", "id"});
  for (bool retry : {true, false}) {
    mcp_->add_tool({retry ? "outbox_retry" : "outbox_discard",
                    retry ? "Tries a dead element again, from the first attempt, for ten more minutes."
                          : "Deletes a dead element for good.",
                    outbox_ref, [this, retry](const bj::object& args) {
                      auto id = args.if_contains("id");
                      if (!id || !id->is_int64()) return ToolResult::fail("id is required");
                      if (!outbox_set(arg(args, "tenant").value_or(""), arg(args, "database").value_or(""), id->as_int64(),
                                      retry)) {
                        return ToolResult::fail("no such dead element");
                      }
                      return ToolResult::ok(bj::object{{retry ? "retrying" : "discarded", id->as_int64()}});
                    }});
  }
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
               if (st.tenant_for(tenant) != tenant) continue;
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
