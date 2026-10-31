// Schemas and tenant databases for operators (design/machine/008.Databases.md §7), and
// the upload check for a manifest's data section.

#include <deque>
#include <functional>
#include <set>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "box_guide.h"
#include "grant_usage.h"
#include "rust_binding.h"
#include "keys.h"
#include "goober/host/host.h"
#include "timer_state.h"
#include "goober/libraries/libraries.h"
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
  // Libraries the host serves (design/machine/831.Libraries.md § 2).
  for (const auto& l : m.libraries) {
    if (!libraries::find(l.library, l.major)) {
      std::string have;
      for (const auto& x : libraries::all()) have += (have.empty() ? "" : ", ") + x.name + "@" + std::to_string(x.major);
      *error = "libraries." + l.alias + ": this host has no " + l.library + "@" + std::to_string(l.major) + " (it has " + have + ")";
      return false;
    }
  }
  // Services spoken in 034's language: the pinned spec is stored, every op resolves in it, and a
  // connection's result route exists.
  for (const auto& svc : m.external) {
    std::string where = "external." + svc.name;
    if (!svc.connect.kind.empty()) {
      bool found = false;
      for (const auto& r : m.results) found |= r.name == svc.connect.result_route;
      if (!found) {
        *error = where + ".connect.result-route names " + svc.connect.result_route + ", which isn't a routes.result route of this box";
        return false;
      }
    }
    if (svc.spec.empty()) continue;
    std::string err;
    auto spec = specs_ ? specs_->get(svc.spec, &err) : nullptr;
    if (!spec) {
      *error = where + ".spec: " + err;
      return false;
    }
    auto resolves = [&](const std::string& ref, const std::string& what) {
      if (ref.empty() || spec->find(ref)) return true;
      *error = where + "." + what + ": " + svc.spec + " has no operation " + ref + " (spec_get lists them)";
      return false;
    };
    for (const auto& [name, em] : svc.methods) {
      std::string mw = "methods." + name;
      if (!resolves(em.op, mw + ".op") || !resolves(em.poll.op, mw + ".poll.op") || !resolves(em.upload.init.op, mw + ".upload.init.op") ||
          !resolves(em.upload.append.op, mw + ".upload.append.op") || !resolves(em.upload.finalize.op, mw + ".upload.finalize.op")) {
        return false;
      }
    }
    if (!svc.connect.check.empty() && !svc.methods.count(svc.connect.check) && !resolves(svc.connect.check, "connect.paste.check")) {
      return false;
    }
  }
  auto active = boxes_->active();
  // Invites grant only roles the host config allows for every tenant the box serves, and a
  // list's title is one thing per tenant, whichever box declares it (029 §§ 2.6, 4).
  {
    auto cfg = config_.get();
    auto served = tenants_of(*cfg, m);
    for (const auto& svc : m.external) {
      for (const auto& role : svc.invite_roles) {
        for (const auto& [tenant, domain] : served) {
          const auto& allowed = cfg->tenant(tenant).invites.roles;
          if (std::find(allowed.begin(), allowed.end(), role) == allowed.end()) {
            *error = "external." + svc.name + ".invites.roles: " + role + " isn't in the host config's tenants." + tenant +
                     ".invites.roles, so this box's invites can't grant it there";
            return false;
          }
        }
      }
      for (const auto& [list, l] : svc.mail_lists) {
        for (const auto& [name, box] : active->boxes) {
          if (name == m.box) continue;
          bool shared = false;
          for (const auto& [t, d] : tenants_of(*cfg, box->manifest)) {
            for (const auto& [mine, md] : served) shared |= t == mine;
          }
          if (!shared) continue;
          for (const auto& other : box->manifest.external) {
            auto it = other.mail_lists.find(list);
            if (it != other.mail_lists.end() && it->second.title != l.title) {
              *error = "external." + svc.name + ".mail.lists." + list + ": box " + name + " already declares it as \"" +
                       it->second.title + "\" for a tenant this box serves; one list, one title";
              return false;
            }
          }
        }
      }
    }
  }
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
      // A callee runs while its caller's transaction is open; on a database they share, the
      // callee's writes would wait on the caller's lock until they failed (016 §5).
      for (const auto& mine : man->data) {
        for (const auto& theirs : callee->second->data) {
          if (!mine.applies_schema() || !theirs.applies_schema() || mine.database != theirs.database) continue;
          if (!mine.tenant.empty() && !theirs.tenant.empty() && mine.tenant != theirs.tenant) continue;
          return *error = where + " calls box " + svc.box + ", and both use database " + mine.database +
                          "; a box can't call one that shares its database, since the call would wait on the caller's "
                          "own transaction. Share the database or call the box, not both.",
                 false;
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

thread_local const std::map<std::string, db::Schema>* Host::pending_schemas_ = nullptr;

bj::object Host::schema_step_report(const db::Schema& s) const {
  bj::object out;
  if (s.follows.empty()) return out;
  auto from = schemas_->get(s.follows);
  if (!from) return out;
  auto step = db::plan_step(*from, s);
  out["step"] = step.to_json();
  bool stored = schemas_->get(s.name).has_value();
  out["held"] = step.needs_person() && !schemas_->approval(s.name);
  if (step.needs_person()) {
    out["approval"] = schemas_->approval(s.name)
                          ? "approved by a person in the panel"
                          : "it drops or rewrites data, so it applies nowhere until a person approves it in the panel's "
                            "Database view" + std::string(stored ? "" : ", once it is stored");
  }
  if (!step.forced_tables.empty()) {
    out["upgrade"] = "forced tables are carried over by an upgrade box (routes.upgrade, from " + from->name + " to " +
                     s.name + "): upgrade_dry_run, then upgrade_start";
  }
  bj::array dbs;
  for (const auto& tenant : databases_->tenants()) {
    for (const auto& database : databases_->databases(tenant)) {
      auto applied = databases_->applied_schemas(tenant, database);
      if (std::find(applied.begin(), applied.end(), from->name) == applied.end()) continue;
      dbs.push_back(databases_->step_impact(tenant, database, step, *from));
    }
  }
  out["databases"] = std::move(dbs);
  return out;
}

std::optional<db::Schema> Host::schema_named(const std::string& name) const {
  if (pending_schemas_) {
    if (auto it = pending_schemas_->find(name); it != pending_schemas_->end()) return it->second;
  }
  return schemas_->get(name);
}

bool Host::check_data(const manifest::Manifest& m, std::string* error) const {
  std::set<std::string> tenants;
  for (const auto& [d, e] : config_.get()->domains) tenants.insert(e.tenant);
  for (const auto& t : databases_->tenants()) tenants.insert(t);
  for (const auto& st : m.data) {
    if (!st.applies_schema()) continue;
    std::string where = "data.sqlite3." + st.name;
    auto schema = schema_named(st.schema);
    if (!schema) {
      return *error = where + ": schema " + st.schema +
                      " isn't stored on this host. From a source project, list its file under \"schemas\" in "
                      "goober.project.json: a build checks it in place, storing nothing, and a deploy stores it. "
                      "Otherwise try it with schema_check, then store it with schema_put",
             false;
    }
    if (schemas_->retired(st.schema)) {
      return *error = where + ": schema " + st.schema + " is retired (a person retired it in the panel, 008 § 11); "
                      "name a schema in service",
             false;
    }
    if (schemas_->held(st.schema)) {
      return *error = where + ": schema " + st.schema + " drops or rewrites data (schema_check shows its step), so a "
                      "person approves it in the panel's Database view before any box may name it "
                      "(design/machine/041.SchemaEvolution.md § 6)",
             false;
    }
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
        if (auto s = schema_named(other.schema)) together.push_back(&held.emplace_back(std::move(*s)));
      }
    }
    if (!together.empty()) {
      together.push_back(&*schema);
      if (!db::Databases::compatible(together, error, schemas_.get())) {
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
  grant_usage_ = std::make_unique<GrantUsage>(started_ms_);
  mcp_->add_tool(
      {"grant_usage",
       "What each active box's data strategies grant, table by table, against what its statements actually did since "
       "the host started (tables, columns, insert, update, delete), with findings: tables granted and never touched, "
       "writes granted and never made, and read: true or update: true where the columns used would do (a suggested "
       "list). Narrow a box's grants to these before its next version. Use is counted in memory since the host "
       "started, so \"unused\" means \"not yet\" for routes that run rarely. A read tool.",
       mcp::object_schema({{"box", mcp::string_property("Only this box, by name, as box_list shows it.")}}, {}), [this](const bj::object& args) {
         auto only = arg(args, "box");
         bj::array out;
         for (const auto& [name, box] : boxes_->active()->boxes) {
           if (only && *only != name) continue;
           bool sql = false;
           for (const auto& st : box->manifest.data) sql |= st.runnable();
           if (!sql) continue;
           out.push_back(grant_report(box->manifest, box->version, grant_usage_->of(name, box->version),
                                      grant_usage_->since_ms(), schemas_.get()));
         }
         if (only && out.empty()) return ToolResult::fail("no active box named " + *only + " with sql strategies");
         return ToolResult::ok(std::move(out));
       }});
  mcp_->add_tool({"box_guide",
                  "Read this first when building a product: how boxes, manifests, routes, MCP tools, timers, jobs, "
                  "flags, databases, ids, mail, and types work on goober, and how to deploy them. For pages, "
                  "templates, and partials, read site_guide. A read tool.",
                  mcp::object_schema({}, {}), [](const bj::object&) { return ToolResult::ok(bj::value(kBoxGuide)); }});
  mcp_->add_tool({"rust_binding",
                  "The Rust source of goober.rs, a ready binding for writing boxes in Rust (Request, respond_json, "
                  "sql, call, flag), with the Cargo.toml and build command to use it. Save it as src/goober.rs in your "
                  "crate; a source project made with source_create {template: \"rust-box\"} already has it. A read "
                  "tool.",
                  mcp::object_schema({}, {}), [](const bj::object&) {
                    return ToolResult::ok(bj::object{
                        {"file", "src/goober.rs"},
                        {"source", kRustBinding},
                        {"cargo_toml",
                         "[package]\nname = \"mybox\"\nversion = \"0.1.0\"\nedition = \"2024\"\npublish = false\n\n"
                         "[lib]\ncrate-type = [\"cdylib\"]\n\n[dependencies]\nserde_json = \"1\"\n\n"
                         "[profile.release]\nopt-level = \"s\"\nlto = true\npanic = \"abort\"\n"},
                        {"lib_rs_starts_with", "mod goober;\nuse goober::*;\n"},
                        {"build", "rustup target add wasm32-unknown-unknown && cargo build --release --target "
                                  "wasm32-unknown-unknown"},
                        {"upload", "version_upload with manifest and wasm_base64 = base64 of "
                                   "target/wasm32-unknown-unknown/release/mybox.wasm"}});
                  }});
  // Keys for external services (015 §5): write-only.
  auto scope_props = [](std::initializer_list<std::pair<std::string, bj::value>> extra) {
    bj::object props{{"scope", mcp::enum_property("system: one key every tenant's boxes may name (keys/system/NAME); "
                                                  "tenant: one tenant's own (keys/tenant/TENANT/NAME).",
                                                  {"system", "tenant"})},
                     {"tenant", mcp::string_property("The tenant, like example; required when scope is tenant.")},
                     {"name", mcp::string_property("The key's name, like stripe or openai.")}};
    for (const auto& [k, v] : extra) props[k] = v;
    return bj::object{{"type", "object"}, {"properties", props}, {"required", bj::array{"scope", "name"}}};
  };
  mcp_->add_tool({"key_set",
                  "Sets a credential that boxes name in external.$service.credentials, like keys/system/openai or "
                  "keys/tenant/{!tenant}/stripe, and the hosts it may be sent to (required): a box calling anywhere "
                  "else gets an error, not the key. Write-only: no tool ever reads a value back, and boxes never see "
                  "it; key_list shows a fingerprint. Setting a name again replaces it. The host's own keys (aws_*, "
                  "backup_key) are refused here: a person sets them on the server with `goober-host key set`. A "
                  "changing tool.",
                  scope_props({{"value", mcp::string_property("The secret, 1 to 8192 printable bytes.")},
                               {"hosts", bj::value(bj::object{{"type", "array"},
                                                              {"items", bj::object{{"type", "string"}}},
                                                              {"description", "Required: the hosts it may be sent to, "
                                                                              "like api.resend.com, or *.example.com."}})}}),
                  [this](const bj::object& args) {
                    std::string err;
                    // The host's own keys are set on the machine, never by an agent: whoever sets the AWS
                    // keys chooses where mail and backups go, and the backup key decides who can read them.
                    std::string name = arg(args, "name").value_or("");
                    if (arg(args, "scope") == "system" && (name.starts_with("aws_") || name == "backup_key")) {
                      return ToolResult::fail(name + " is the host's own key: set it on the server with `goober-host key set " +
                                              name + "`");
                    }
                    std::vector<std::string> hosts;
                    if (auto* h = args.if_contains("hosts"); h && h->is_array()) {
                      for (const auto& v : h->as_array()) {
                        if (v.is_string()) hosts.emplace_back(v.as_string());
                      }
                    }
                    if (hosts.empty()) return ToolResult::fail("hosts is required: where the key may be sent");
                    if (!keys_->set(arg(args, "scope").value_or(""), arg(args, "tenant").value_or(""),
                                    arg(args, "name").value_or(""), arg(args, "value").value_or(""), base::now_unix_ms(),
                                    &err, hosts)) {
                      return ToolResult::fail(err);
                    }
                    return ToolResult::ok(bj::object{{"set", true}});
                  }});
  mcp_->add_tool({"key_list",
                  "The keys that are set, system and per tenant: names, when, and a fingerprint, never values. A read "
                  "tool.",
                  mcp::object_schema({}, {}), [this](const bj::object&) { return ToolResult::ok(keys_->list()); }});
  mcp_->add_tool({"key_delete",
                  "Removes a key. Box calls that name it fail until it is set again (key_set). The host's own keys "
                  "(aws_*, backup_key) are refused. A changing tool.",
                  scope_props({}),
                  [this](const bj::object& args) {
                    std::string name = arg(args, "name").value_or("");
                    if (arg(args, "scope") == "system" && (name.starts_with("aws_") || name == "backup_key")) {
                      return ToolResult::fail(name + " is the host's own key: it is managed on the server");
                    }
                    if (!keys_->remove(arg(args, "scope").value_or(""), arg(args, "tenant").value_or(""),
                                       arg(args, "name").value_or(""))) {
                      return ToolResult::fail("no such key");
                    }
                    return ToolResult::ok(bj::object{{"removed", true}});
                  }});
  // Backups to S3 (design/machine/019.Backups.md).
  mcp_->add_tool({"backup_status",
                  "Backups to S3: whether they're on and healthy, the current night and last increment, the attachment "
                  "queue, the source mirror, the last error, whether a person has confirmed keeping a copy of the "
                  "backup key (only a person can, on the server), and, with points, the restore points in the bucket. "
                  "A read tool.",
                  mcp::object_schema({{"points", mcp::boolean_property("Also list the restore points in the bucket.")}}, {}),
                  [this](const bj::object& args) {
                    bool points = args.contains("points") && args.at("points").is_bool() && args.at("points").as_bool();
                    return ToolResult::ok(backup_status(points));
                  }});
  mcp_->add_tool({"backup_run",
                  "Runs one backup stream now instead of on its schedule: nightly (a whole sealed archive; increments "
                  "then measure against it), increment (the databases' changed pages and changed files), assets "
                  "(queued attachments), or source (source projects' patches and deployed trees not yet in the "
                  "bucket). Answers what went up, or why it couldn't run (backups off, no AWS keys, a path it can't "
                  "read). A changing tool.",
                  mcp::object_schema({{"what", mcp::enum_property("Which stream.", {"nightly", "increment", "assets", "source"})}},
                                     {"what"}),
                  [this](const bj::object& args) {
                    std::string err;
                    auto r = backup_run(arg(args, "what").value_or(""), base::now_unix_ms(), &err);
                    if (!r) return ToolResult::fail(err);
                    return ToolResult::ok(std::move(*r));
                  }});
  // The outbox's dead letters (035): what couldn't be delivered, and what to do about it.
  mcp_->add_tool({"outbox_dead_letters",
                  "Effectful calls the outbox gave up on (mail, API calls with effects): tenant, database, id, which "
                  "box, service, and request, how many attempts, and the last status or error. Then outbox_retry or "
                  "outbox_discard with that tenant, database, and id. A read tool.",
                  mcp::object_schema({}, {}), [this](const bj::object&) { return ToolResult::ok(outbox_list("dead")); }});
  auto outbox_ref = mcp::object_schema({{"tenant", mcp::string_property("The tenant.")},
                                        {"database", mcp::string_property("The database, as outbox_dead_letters shows it.")},
                                        {"id", mcp::integer_property("The element's id, a number, as outbox_dead_letters shows it.")}},
                                       {"tenant", "database", "id"});
  for (bool retry : {true, false}) {
    mcp_->add_tool({retry ? "outbox_retry" : "outbox_discard",
                    retry ? "Tries a dead outbox element again, from its first attempt, for ten more minutes, with "
                            "the same idempotency key. A changing tool."
                          : "Deletes a dead outbox element for good: it is never sent. A changing tool.",
                    outbox_ref, [this, retry](const bj::object& args) {
                      // A number, or a string of digits.
                      int64_t n = -1;
                      if (const auto* id = args.if_contains("id"); id && id->is_int64()) {
                        n = id->as_int64();
                      } else if (id && id->is_string() && !id->as_string().empty() && id->as_string().size() <= 18 &&
                                 std::string_view(id->as_string()).find_first_not_of("0123456789") == std::string_view::npos) {
                        n = std::atoll(std::string(id->as_string()).c_str());
                      }
                      if (n < 0) return ToolResult::fail("id is required: the number outbox_dead_letters shows");
                      if (!outbox_set(arg(args, "tenant").value_or(""), arg(args, "database").value_or(""), n, retry)) {
                        return ToolResult::fail("no such dead element");
                      }
                      return ToolResult::ok(bj::object{{retry ? "retrying" : "discarded", n}});
                    }});
  }
  mcp_->add_tool({"timers_list",
                  "The timers the host has planned (045): each timer route, tenant, and source row, with when it is "
                  "next due. A read tool.",
                  mcp::object_schema({}, {}),
                  [this](const bj::object&) { return ToolResult::ok(timers_due()); }});
  mcp_->add_tool({"timer_run",
                  "Fires a timer now for one tenant, for real, as its schedule would: once, or once per active row "
                  "of its by_timer source. Its writes commit and its effects are queued. Answers {fired}: how many "
                  "calls succeeded. For trying a timer without waiting for its time. A changing tool.",
                  mcp::object_schema({{"timer", mcp::string_property("The timer's route name, like daily_digest.")},
                                      {"tenant", mcp::string_property("A tenant, like example.")}},
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

  auto tenant_prop = string_property("A tenant name, like example.");
  auto db_prop = string_property("A database name, like guestbook: a strategy's \"database\".");

  mcp_->add_tool(
      {"schema_put",
       "Stores a database schema: {\"schema\": \"name1\", \"tables\": {\"t\": {\"columns\": {\"c\": {\"type\": "
       "\"string\"}}, \"indexes\": {...}}}}, for good. Types are id, int32, int64, double, float, boolean, and "
       "string; columns take nullable, default, max-length, primary-key, and not-a-reference. Key every table with "
       "\"id\": {\"type\": \"id\", \"primary-key\": true}: the host stamps it from its id service, and a box never "
       "names one. The primary key is never a natural key (a string like address|day): make that a unique index on "
       "its own columns, and insert with ON CONFLICT on it. A column that refers to a user or a row, in any "
       "database, is {\"type\": \"id\"}; never text, never an email. Ids read back as strings. A schema is a "
       "version: its name is never reused with different content (the same content again is a no-op). A schema "
       "without \"follows\" only adds to the tables it names. The next version of a schema names the one it "
       "replaces, \"follows\": \"name1\", and its tables replace that one's: what it leaves out is dropped, and "
       "each change is classified (design/machine/041.SchemaEvolution.md): expansion, indexing, and emulated "
       "(int32 to int64, float to double, allowing null, a longer max-length) apply when a box names the version; "
       "contraction (a dropped column or table: older boxes then read its default and their writes to it are "
       "dropped) and forced (anything else, carried over by an upgrade box's rows handler) wait for a person to "
       "approve the schema in the panel; a primary key never changes. Try it with schema_check first, which "
       "stores nothing and says what it would do to each database. Answers {schema, tables, warnings?, step?, "
       "held?, approval?, upgrade?, databases?}: the warnings name keys and references that aren't ids "
       "(\"not-a-reference\": true silences one on a column that only looks like a reference, like a "
       "message_id); step lists each change with a sentence and 420's consequences. A changing tool: it stores "
       "the schema, and nothing goes live until a box version names it.",
       object_schema({{"schema", any_property("The schema, as a JSON object or JSON text.")}}, {"schema"}),
       [this](const bj::object& args) {
         auto it = args.find("schema");
         if (it == args.end()) return ToolResult::fail("schema is required");
         std::string text = it->value().is_string() ? std::string(it->value().get_string()) : json::dump(it->value());
         std::string err;
         auto s = schemas_->put(text, &err);
         if (!s) return ToolResult::fail(err);
         bj::array tables, warnings;
         for (const auto& t : s->tables) tables.push_back(bj::value(t.name));
         for (const auto& w : s->warnings) warnings.push_back(bj::value(w));
         bj::object out{{"schema", s->name}, {"tables", std::move(tables)}};
         if (!warnings.empty()) out["warnings"] = std::move(warnings);
         for (auto& kv : schema_step_report(*s)) out[kv.key()] = kv.value();
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"schema_check",
       "A dry run of schema_put: checks a schema exactly as schema_put would (its shape, the types and keys, that its "
       "name is free or holds the same content, and that what it follows is stored and followed by nothing else) "
       "and answers {schema, ok, stored, note, tables, warnings}, storing nothing; for a schema that follows "
       "another, also step (each change, its kind, a sentence, and 420's consequences), held, approval, upgrade, "
       "and databases: each database holding the version it follows, with the rows holding data a contraction "
       "would drop (drops) and the rows a forced table would carry over (forced). A schema name is never "
       "reused, so try it here first. To check a manifest against schemas not "
       "stored yet, pass them to version_upload {dry_run: true, schemas: [...]}; a source build checks a project's "
       "own schemas the same way when goober.project.json lists them under \"schemas\". A read tool.",
       object_schema({{"schema", any_property("The schema, as a JSON object or JSON text.")}}, {"schema"}),
       [this](const bj::object& args) {
         auto it = args.find("schema");
         if (it == args.end()) return ToolResult::fail("schema is required");
         std::string text = it->value().is_string() ? std::string(it->value().get_string()) : json::dump(it->value());
         std::string err;
         auto s = schemas_->check(text, &err);
         if (!s) return ToolResult::fail(err);
         bj::array tables, warnings;
         for (const auto& t : s->tables) tables.push_back(bj::value(t.name));
         for (const auto& w : s->warnings) warnings.push_back(bj::value(w));
         bool stored = schemas_->get(s->name).has_value();
         bj::object out{{"schema", s->name},
                        {"ok", true},
                        {"stored", stored},
                        {"note", stored ? "already stored with this same content: schema_put is a no-op"
                                        : "not stored: schema_put stores it, for good, under this name"},
                        {"tables", std::move(tables)},
                        {"warnings", std::move(warnings)}};
         for (auto& kv : schema_step_report(*s)) out[kv.key()] = kv.value();
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool({"schema_get", "A stored schema's JSON, exactly as stored. A read tool.",
                  object_schema({{"name", string_property("The schema's name, like orders1.")}}, {"name"}),
                  [this](const bj::object& args) {
                    auto name = arg(args, "name");
                    if (!name) return ToolResult::fail("name is required");
                    auto s = schemas_->get(*name);
                    if (!s) return ToolResult::fail("no schema named " + *name);
                    std::string err;
                    return ToolResult::ok(*json::parse(s->text, &err));
                  }});

  mcp_->add_tool({"schema_list", "The names of the stored schemas. A read tool.", object_schema({}, {}),
                  [this](const bj::object&) {
                    bj::array out;
                    for (const auto& n : schemas_->list()) out.push_back(bj::value(n));
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool(
      {"db_list",
       "Lists every tenant's databases: the active boxes and strategies that use each one, whether its file exists "
       "yet (a box's first request creates it, or db_create does ahead of time), its size, and its tables with row "
       "counts. A read tool.",
       object_schema({{"tenant", string_property("Only this tenant, like example.")}}, {}), [this](const bj::object& args) {
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

  mcp_->add_tool({"db_describe",
                  "Describes one tenant database: tables, columns, indexes, row counts, and the schemas applied. A "
                  "read tool.",
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
       "Runs one read-only SQL statement on a tenant's database and returns {columns, rows}. Values read straight "
       "from id columns are strings; computed ones, like max(id), are numbers. Unlike a box's SQL, any SQLite "
       "function may be called. A read tool.",
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
       "Runs one INSERT, UPDATE, or DELETE on a tenant's database, at once, and returns {changes, last_insert_id}, "
       "plus {ids, id} (strings) for rows inserted into a table keyed by id. Leave such a key out: the host stamps "
       "it, and naming one is refused, for operators as for boxes. Schema changes and the host's goober_ tables "
       "are refused. Every call is logged to logs/db.log with the operator. A changing tool.",
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
         log_line("db", (bj::object{{"at", base::format_utc_ms(base::now_unix_ms())},
                                                 {"operator", current_operator()},
                                                 {"tenant", *tenant},
                                                 {"database", *database},
                                                 {"sql", *sql},
                                                 {"params", params},
                                                 {"changes", r ? r->at("changes") : bj::value(nullptr)},
                                                 {"error", r ? "" : err}}));
         if (!r) return ToolResult::fail(err);
         bj::object out{{"changes", r->at("changes")}, {"last_insert_id", r->at("last_insert_id")}};
         for (const char* k : {"ids", "id"}) {
           if (const auto* v = r->if_contains(k)) out[k] = *v;
         }
         return ToolResult::ok(std::move(out));
       }});
}

}  // namespace goober::host
