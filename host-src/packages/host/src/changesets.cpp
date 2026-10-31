// Change sets (design/machine/042.ChangeSets.md): schema adds, box versions, content, and flag
// values grouped as one change, previewed and reviewed together, applied together in a fixed
// order (schemas, versions, content, flags) with what each replaced kept first, and rolled back
// together by one call. Each member runs through the operator tool that does it alone
// (schema_put, version_upload, version_promote, content_deploy, flag_set), so a change set
// checks nothing less than those do.

#include <algorithm>
#include <filesystem>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/boxes/review.h"
#include "goober/host/host.h"
#include "goober/operations/changes.h"
#include "flags.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {

constexpr size_t kMaxSchemas = 20, kMaxVersions = 20, kMaxContent = 10, kMaxFlags = 50, kMaxTries = 20;
constexpr size_t kMaxBytes = size_t(48) << 20;

std::string text(const bj::object& o, const char* k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

bool flag_of(const bj::object& o, const char* k) {
  auto* v = o.if_contains(k);
  return v && v->is_bool() && v->as_bool();
}

// A schema given as an object or as JSON text, as JSON text.
std::string schema_text(const bj::value& v) { return v.is_string() ? std::string(v.as_string()) : json::dump(v); }

// The key a member is replaced by when added again.
std::string member_key(const bj::object& m) {
  std::string kind = text(m, "kind");
  if (kind == "schema") return "schema:" + text(m, "name");
  if (kind == "version") return "version:" + text(m, "box");
  if (kind == "content") return "content:" + text(m, "tenant");
  return "flag:" + text(m, "tenant") + "/" + text(m, "box") + "/" + text(m, "flag");
}

int kind_order(const std::string& kind) {
  if (kind == "schema") return 0;
  if (kind == "version") return 1;
  if (kind == "content") return 2;
  return 3;
}

// What one internal tool call answered: the value, or the refusal as error.
struct Called {
  bool ok = false;
  bj::value value;
  std::string error;
};

}  // namespace

fs::path Host::changeset_file(const std::string& id) const {
  return config_.get()->storage / "operations" / "changesets" / (id + ".json");
}

std::optional<bj::object> Host::load_changeset(const std::string& id) {
  if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos || id.size() > 20) return std::nullopt;
  auto t = base::read_file(changeset_file(id));
  if (!t) return std::nullopt;
  std::string err;
  auto v = json::parse(*t, &err);
  if (!v || !v->is_object()) return std::nullopt;
  return std::move(v->as_object());
}

bool Host::save_changeset(const bj::object& cs, std::string* error) {
  auto file = changeset_file(text(cs, "id"));
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  std::string body = json::dump(cs);
  if (body.size() > kMaxBytes + (1 << 20)) return *error = "a change set holds at most 48 MiB", false;
  if (!base::write_file_durable(file, body)) return *error = "couldn't save the change set", false;
  return true;
}

// Whether this operator may see a change set: their own, or any when they aren't focused (030).
bool Host::changeset_visible(const bj::object& cs) {
  std::string op = current_operator();
  return operator_scope(op).all || scope_key(text(cs, "by")) == scope_key(op);
}

// Each member as the tool call that does it alone, for the operator's scope (030): a focused
// operator may group only what they could do one call at a time.
std::optional<std::string> Host::changeset_scope_refusal(const bj::object& member) {
  std::string op = current_operator();
  auto scope = operator_scope(op);
  if (scope.all) return std::nullopt;
  std::string kind = text(member, "kind");
  if (kind == "schema") return scope_refusal(scope, op, "schema_put", bj::object{{"schema", member.at("schema")}});
  if (kind == "version") {
    if (auto* u = member.if_contains("upload"); u && u->is_object()) {
      if (auto r = scope_refusal(scope, op, "version_upload", bj::object{{"manifest", u->as_object().at("manifest")}})) return r;
    } else if (auto r = scope_refusal(scope, op, "version_promote",
                                      bj::object{{"box", text(member, "box")}, {"version", text(member, "version")}})) {
      return r;
    }
    if (auto* tries = member.if_contains("tries"); tries && tries->is_array()) {
      for (const auto& t : tries->as_array()) {
        if (auto r = scope_refusal(scope, op, "route_try", t.as_object())) return r;
      }
    }
    return std::nullopt;
  }
  if (kind == "content") return scope_refusal(scope, op, "content_deploy", bj::object{{"tenant", text(member, "tenant")}});
  return scope_refusal(scope, op, "flag_set", bj::object{{"tenant", text(member, "tenant")}, {"box", text(member, "box")}});
}

// The manifest a box will run once the change set's versions are live: the member's version,
// else the live one.
std::optional<manifest::Manifest> Host::changeset_manifest(const bj::object& cs, const std::string& box) {
  std::string err;
  for (const auto& mv : cs.at("members").as_array()) {
    const auto& m = mv.as_object();
    if (text(m, "kind") != "version" || text(m, "box") != box) continue;
    if (auto* u = m.if_contains("upload"); u && u->is_object()) {
      return manifest::parse_manifest(schema_text(u->as_object().at("manifest")), &err);
    }
    auto t = boxes_->manifest_text(box, text(m, "version"));
    return t ? manifest::parse_manifest(*t, &err) : std::nullopt;
  }
  auto active = boxes_->active();
  auto it = active->boxes.find(box);
  if (it == active->boxes.end()) return std::nullopt;
  return it->second->manifest;
}

bj::object Host::changeset_preview(const bj::object& cs, bool run_tries) {
  auto call = [this](const std::string& tool, const bj::object& args) {
    Called c;
    auto r = mcp_->call(tool, args);
    if (!r) return c.error = "no tool " + tool, c;
    if (r->is_error) return c.error = r->value.is_string() ? std::string(r->value.as_string()) : json::dump(r->value), c;
    c.ok = true;
    c.value = std::move(r->value);
    return c;
  };
  bool look_only = !may_change(operator_roles(current_operator()));
  bj::array members, problems, widens;
  // Schemas the set stores, for checking its uploads as if they were stored already.
  bj::array pending;
  for (const auto& mv : cs.at("members").as_array()) {
    const auto& m = mv.as_object();
    if (text(m, "kind") == "schema" && !schemas_->get(text(m, "name"))) pending.push_back(m.at("schema"));
  }
  auto problem = [&](const std::string& key, const std::string& why) { problems.push_back(bj::value(key + ": " + why)); };
  for (const auto& mv : cs.at("members").as_array()) {
    const auto& m = mv.as_object();
    std::string kind = text(m, "kind"), key = member_key(m);
    bj::object out{{"kind", kind}, {"key", key}};
    if (auto refused = changeset_scope_refusal(m)) {
      problem(key, *refused);
      out["ok"] = false;
      members.push_back(std::move(out));
      continue;
    }
    bool ok = true;
    if (kind == "schema") {
      std::string err;
      auto s = schemas_->check(schema_text(m.at("schema")), &err);
      if (!s) {
        ok = false;
        problem(key, err);
      } else {
        bool stored = schemas_->get(s->name).has_value();
        out["stored_already"] = stored;
        out["note"] = stored ? "already stored with this content: nothing to do" : "stored for good when applied";
        bj::array tables, warnings;
        for (const auto& t : s->tables) tables.push_back(bj::value(t.name));
        for (const auto& w : s->warnings) warnings.push_back(bj::value(w));
        out["tables"] = std::move(tables);
        if (!warnings.empty()) out["warnings"] = std::move(warnings);
      }
    } else if (kind == "version") {
      std::string box = text(m, "box");
      auto active = boxes_->active();
      auto live = active->boxes.find(box);
      std::string from = live != active->boxes.end() ? live->second->version : "";
      if (from.empty()) {
        if (auto d = boxes_->disabled(box)) from = d->version;
      }
      out["box"] = box;
      out["live"] = live != active->boxes.end() ? bj::value(live->second->version) : bj::value(nullptr);
      bj::value review;
      if (auto* u = m.if_contains("upload"); u && u->is_object()) {
        bj::object args = u->as_object();
        args["dry_run"] = true;
        if (!pending.empty()) args["schemas"] = pending;
        auto c = call("version_upload", args);
        if (!c.ok) {
          ok = false;
          problem(key, c.error);
        } else if (auto* r = c.value.as_object().if_contains("review")) {
          review = *r;
        }
        out["version"] = "(stored dark when applied)";
      } else {
        std::string version = text(m, "version");
        out["version"] = version;
        auto to = boxes_->files(box, version);
        if (!to) {
          ok = false;
          problem(key, "box " + box + " has no stored version " + version);
        } else {
          std::optional<boxes::VersionFiles> a;
          if (!from.empty()) a = boxes_->files(box, from);
          std::string err;
          if (auto r = boxes::review(a ? &*a : nullptr, *to, &err)) review = std::move(*r);
          if (version == from && live != active->boxes.end()) out["note"] = "already live: applying it changes nothing";
        }
        // The tries the member carries, against that version, as route_try runs them.
        if (ok && run_tries) {
          bj::array tried;
          if (auto* tries = m.if_contains("tries"); tries && tries->is_array()) {
            for (const auto& tv : tries->as_array()) {
              bj::object args = tv.as_object();
              args["box"] = box;
              args["version"] = version;
              std::string label = text(args, "path").empty() ? "tool " + text(args, "tool") : text(args, "method") + " " + text(args, "path");
              if (look_only && !is_read_call("route_try", args)) {
                tried.push_back(bj::object{{"try", label}, {"skipped", "a look-only caller tries GETs only"}});
                continue;
              }
              auto c = call("route_try", args);
              bj::object row{{"try", label}};
              if (!c.ok) {
                ok = false;
                row["error"] = c.error;
                problem(key, "try " + label + ": " + c.error);
              } else {
                const auto& r = c.value.as_object();
                for (const char* k : {"status", "would_change", "would_queue", "smtp"}) {
                  if (auto* v = r.if_contains(k)) row[k] = *v;
                }
                if (auto* s = r.if_contains("status"); s && s->is_int64() && s->as_int64() >= 500) {
                  ok = false;
                  problem(key, "try " + label + " answered " + std::to_string(s->as_int64()));
                }
              }
              tried.push_back(std::move(row));
            }
          }
          if (!tried.empty()) out["tries"] = std::move(tried);
        }
      }
      if (review.is_object()) {
        if (auto* w = review.as_object().if_contains("widens"); w && w->is_array()) {
          for (const auto& s : w->as_array()) widens.push_back(bj::value(box + ": " + std::string(s.as_string())));
        }
        out["review"] = std::move(review);
      }
    } else if (kind == "content") {
      bj::object args{{"tenant", text(m, "tenant")}, {"dry_run", true}};
      for (const char* k : {"pages", "files"}) {
        if (auto* v = m.if_contains(k); v && v->is_array() && !v->as_array().empty()) args[k] = *v;
      }
      auto c = call("content_deploy", args);
      if (!c.ok) {
        ok = false;
        problem(key, c.error);
      } else {
        const auto& r = c.value.as_object();
        out["lines"] = r.at("lines");
        if (auto* o = r.if_contains("ok"); !o || !o->is_bool() || !o->as_bool()) {
          ok = false;
          problem(key, "the content dry run found problems (its lines say which)");
        }
      }
    } else {
      std::string tenant = text(m, "tenant"), box = text(m, "box"), name = text(m, "flag"), value = text(m, "value");
      auto target = changeset_manifest(cs, box);
      out["flag"] = name;
      out["value"] = value;
      const manifest::Manifest::Flag* f = target ? target->flag(name) : nullptr;
      bool serves = false;
      if (target) {
        for (const auto& [tn, dm] : tenants_of(*config_.get(), *target)) serves |= tn == tenant;
        for (const auto& d : target->data) serves |= d.applies_schema() && d.tenant == tenant;
      }
      if (!target) {
        ok = false;
        problem(key, "box " + box + " isn't live and the change set has no version of it");
      } else if (!f) {
        ok = false;
        problem(key, "box " + box + " (as it will be) declares no flag " + name);
      } else if (!serves) {
        ok = false;
        problem(key, "box " + box + " doesn't serve tenant " + tenant);
      } else if (!f->values.empty() && std::find(f->values.begin(), f->values.end(), value) == f->values.end()) {
        ok = false;
        std::string allowed;
        for (const auto& v : f->values) allowed += (allowed.empty() ? "" : ", ") + v;
        problem(key, name + " takes one of: " + allowed);
      } else {
        auto active = boxes_->active();
        auto live = active->boxes.find(box);
        std::optional<std::string> now;
        if (live != active->boxes.end()) now = flag_for(live->second->manifest, tenant, name);
        out["before"] = now ? *now : f->default_value;
      }
    }
    out["ok"] = ok;
    members.push_back(std::move(out));
  }
  bj::object result{{"id", text(cs, "id")},
                    {"title", text(cs, "title")},
                    {"state", text(cs, "state")},
                    {"draft_version", cs.at("version")},
                    {"ok", problems.empty() && !members.empty()},
                    {"stored", false},
                    {"members", std::move(members)},
                    {"widens", std::move(widens)},
                    {"problems", std::move(problems)},
                    {"order", "schemas, then versions, content, and flags; rolled back in the reverse order"}};
  if (cs.at("members").as_array().empty()) result["problems"].as_array().push_back(bj::value("the change set is empty"));
  return result;
}

// One applied member's undo (042 § 5). Answers what was done, or why it couldn't be.
bj::object Host::changeset_undo(const bj::object& step, const std::string& id) {
  auto call = [this](const std::string& tool, const bj::object& args) -> std::string {
    auto r = mcp_->call(tool, args);
    if (!r) return "no tool " + tool;
    if (r->is_error) return r->value.is_string() ? std::string(r->value.as_string()) : json::dump(r->value);
    return "";
  };
  std::string kind = text(step, "kind");
  bj::object out{{"kind", kind}, {"key", step.at("key")}};
  const auto& before = step.at("before").as_object();
  std::string err;
  if (kind == "schema") {
    out["left"] = "schemas only grow: " + text(step, "name") + " stays stored, and columns a version added stay";
  } else if (kind == "version") {
    std::string box = text(step, "box");
    if (!text(before, "active").empty()) {
      err = call("version_promote", bj::object{{"box", box}, {"version", text(before, "active")}});
      out["done"] = "version_promote " + text(before, "active");
    } else if (!text(before, "disabled").empty()) {
      err = call("version_promote", bj::object{{"box", box}, {"version", text(before, "disabled")}});
      if (err.empty()) err = call("box_disable", bj::object{{"box", box}, {"why", text(before, "why")}});
      out["done"] = "back to " + text(before, "disabled") + ", disabled as it was";
    } else {
      err = call("box_disable", bj::object{{"box", box}, {"why", "rolled back with change set " + id}});
      out["done"] = "box_disable (the box wasn't live before)";
    }
  } else if (kind == "content") {
    std::string tenant = text(step, "tenant");
    int restored = 0;
    for (const auto& fv : before.at("files").as_array()) {
      const auto& f = fv.as_object();
      std::string path = text(f, "file");
      auto now = content_->get(tenant, path);
      if (f.contains("base64")) {
        auto data = base::base64_decode(text(f, "base64")).value_or("");
        if (now && now->data == data) continue;
        if (!content_->put(tenant, path, data, &err)) break;
      } else {
        if (!now) continue;
        if (!content_->remove(tenant, path, &err)) break;
      }
      ++restored;
    }
    out["done"] = std::to_string(restored) + " file(s) put back as they were";
  } else {
    err = call("flag_set", bj::object{{"tenant", text(step, "tenant")}, {"box", text(step, "box")}, {"flag", text(step, "flag")},
                                      {"value", text(before, "value")}, {"reason", "rolled back with change set " + id}});
    out["done"] = "set back to " + text(before, "value");
  }
  if (!err.empty()) out["error"] = err;
  return out;
}

void Host::register_changeset_tools() {
  using mcp::boolean_property;
  using mcp::object_schema;
  using mcp::string_property;
  auto id_prop = string_property("The change set's id, as changeset_create answered it (a string of digits).");
  auto list_prop = [](const char* about) {
    return bj::value(bj::object{{"type", "array"}, {"items", bj::object{{"type", "object"}}}, {"description", about}});
  };

  mcp_->add_tool(
      {"changeset_create",
       "Starts a change set: one change made of schema adds, box versions, content, and flag values, previewed, "
       "reviewed, applied, and rolled back together (042). Answers its id (a string). Add members with "
       "changeset_add, look with changeset_preview (stores nothing), then changeset_apply makes it all live in one call "
       "and changeset_rollback undoes it all in one call. Stores only the draft, so it never waits for approval; "
       "changeset_apply does.",
       object_schema({{"title", string_property("What the change is, in a few words, like \"order notes\".")},
                      {"why", string_property("Why, in a sentence: kept with the change set and shown to whoever approves it.")}},
                     {"title"}),
       [this](const bj::object& args) {
         std::string title = text(args, "title");
         if (title.empty() || title.size() > 200) return ToolResult::fail("title is required: a few words, at most 200 bytes");
         int64_t now = base::now_unix_ms();
         std::string id = std::to_string(next_id_());
         bj::object cs{{"id", id},          {"title", title},       {"why", text(args, "why")},
                       {"by", current_operator()}, {"created_ms", now}, {"updated_ms", now},
                       {"state", "draft"},  {"version", 0},         {"members", bj::array{}}};
         std::string err;
         std::lock_guard lock(changesets_mu_);
         if (!save_changeset(cs, &err)) return ToolResult::fail(err);
         return ToolResult::ok(bj::object{{"id", id},
                                          {"title", title},
                                          {"state", "draft"},
                                          {"next", "changeset_add {id, schemas?, versions?, content?, flags?}"}});
       }});

  mcp_->add_tool(
      {"changeset_add",
       "Adds members to a draft change set, each replacing the one with the same key: schemas (schema_put's schema "
       "each; key schema:name), versions (each {box, version} of a version already stored, dark or old, or {manifest, "
       "wat | wasm_base64, types?} to store dark when applied, after the set's schemas; tries?: route_try arguments "
       "without box and version, which changeset_preview runs against a stored version; key version:box), content "
       "(each {tenant, pages?, files?} as content_deploy takes them, merged by path into the tenant's member, or "
       "{tenant, from_stage: true} to copy your content_stage draft in; key content:tenant), and flags (each {tenant, "
       "box, flag, value}; key flag:tenant/box/flag). remove drops members by key (or one content path as "
       "content:tenant:path). Each member is checked for shape and for your scope now, and again when applied. "
       "Answers the members and the draft's version (changeset_apply runs exactly that version). Stores only the "
       "draft, so it never waits for approval.",
       object_schema({{"id", id_prop},
                      {"schemas", list_prop("Schemas to store, as schema_put takes them.")},
                      {"versions", list_prop("Box versions: {box, version, tries?} or {manifest, wat | wasm_base64, types?}.")},
                      {"content", list_prop("Content per tenant: {tenant, pages?, files?} or {tenant, from_stage: true}.")},
                      {"flags", list_prop("Flag values: {tenant, box, flag, value}.")},
                      {"remove", bj::object{{"type", "array"},
                                            {"items", bj::object{{"type", "string"}}},
                                            {"description", "Member keys to drop, like version:orders or content:example:/about."}}}},
                     {"id"}),
       [this](const bj::object& args) {
         std::lock_guard lock(changesets_mu_);
         auto cs = load_changeset(text(args, "id"));
         if (!cs || !changeset_visible(*cs)) return ToolResult::fail("no change set " + text(args, "id"));
         if (text(*cs, "state") != "draft") {
           return ToolResult::fail("change set " + text(*cs, "id") + " is " + text(*cs, "state") + ": only a draft takes members");
         }
         // The members by key, in the order they came.
         std::vector<std::pair<std::string, bj::object>> members;
         for (const auto& mv : cs->at("members").as_array()) members.emplace_back(member_key(mv.as_object()), mv.as_object());
         auto put = [&](bj::object m) {
           std::string key = member_key(m);
           for (auto& [k, v] : members) {
             if (k == key) return void(v = std::move(m));
           }
           members.emplace_back(key, std::move(m));
         };
         auto find = [&](const std::string& key) -> bj::object* {
           for (auto& [k, v] : members) {
             if (k == key) return &v;
           }
           return nullptr;
         };
         auto list = [&](const char* k) -> const bj::array* {
           auto* v = args.if_contains(k);
           return v && v->is_array() ? &v->as_array() : nullptr;
         };
         auto known_tenant = [this](const std::string& t) {
           for (const auto& [d, e] : config_.get()->domains) {
             if (e.tenant == t) return true;
           }
           return false;
         };
         std::string err;
         bj::array added;
         auto refuse = [](const std::string& why) { return ToolResult::fail(why + "; nothing was added"); };
         if (auto* a = list("schemas")) {
           for (const auto& v : *a) {
             auto s = schemas_->check(schema_text(v), &err);
             if (!s) return refuse("schemas: " + err);
             put(bj::object{{"kind", "schema"}, {"name", s->name}, {"schema", schema_text(v)}});
             added.push_back(bj::value("schema:" + s->name));
           }
         }
         if (auto* a = list("versions")) {
           for (const auto& v : *a) {
             if (!v.is_object()) return refuse("each version is {box, version} or {manifest, wat | wasm_base64}");
             const auto& o = v.as_object();
             bj::object m{{"kind", "version"}};
             if (o.contains("manifest")) {
               std::string mt = schema_text(o.at("manifest"));
               auto parsed = manifest::parse_manifest(mt, &err);
               if (!parsed) return refuse("versions: " + err);
               if (o.contains("wat") == o.contains("wasm_base64")) return refuse("versions: give exactly one of wat or wasm_base64");
               bj::object upload{{"manifest", mt}};
               for (const char* k : {"wat", "wasm_base64", "types"}) {
                 if (auto* x = o.if_contains(k)) upload[k] = *x;
               }
               m["box"] = parsed->box;
               m["upload"] = std::move(upload);
               if (o.contains("tries")) return refuse("versions: tries need a stored version (version_upload {activate: false} first)");
             } else {
               std::string box = text(o, "box"), version = text(o, "version");
               if (!boxes_->files(box, version)) return refuse("versions: box " + box + " has no stored version " + version);
               m["box"] = box;
               m["version"] = version;
               if (auto* t = o.if_contains("tries")) {
                 if (!t->is_array() || t->as_array().size() > kMaxTries) return refuse("tries is a list of up to 20 route_try arguments");
                 bj::array tries;
                 for (const auto& tv : t->as_array()) {
                   if (!tv.is_object()) return refuse("each try is route_try's arguments");
                   bj::object tr = tv.as_object();
                   tr.erase("box");
                   tr.erase("version");
                   tries.push_back(std::move(tr));
                 }
                 m["tries"] = std::move(tries);
               }
             }
             added.push_back(bj::value("version:" + text(m, "box")));
             put(std::move(m));
           }
         }
         if (auto* a = list("content")) {
           for (const auto& v : *a) {
             if (!v.is_object()) return refuse("each content item is {tenant, pages?, files?} or {tenant, from_stage: true}");
             bj::object o = v.as_object();
             std::string tenant = text(o, "tenant");
             if (!known_tenant(tenant)) return refuse("content: no domain in the host config serves tenant " + tenant);
             if (flag_of(o, "from_stage")) {
               std::lock_guard stage(content_stage_mu_);
               auto draft = load_content_stage(current_operator(), tenant);
               if (!draft) return refuse("content: no content_stage draft of yours for " + tenant);
               bj::array pages, files;
               for (const auto& [k, pv] : draft->at("pages").as_object()) pages.push_back(pv);
               for (const auto& [k, fv] : draft->at("files").as_object()) files.push_back(fv);
               o["pages"] = std::move(pages);
               o["files"] = std::move(files);
             }
             if (!content_deploy_files(tenant, o, &err)) return refuse("content: " + err);
             bj::object* m = find("content:" + tenant);
             if (!m) {
               put(bj::object{{"kind", "content"}, {"tenant", tenant}, {"pages", bj::array{}}, {"files", bj::array{}}});
               m = find("content:" + tenant);
             }
             // Merged by path: an item here replaces the member's item at its path.
             for (const char* k : {"pages", "files"}) {
               auto* given = o.if_contains(k);
               if (!given || !given->is_array()) continue;
               auto& items = (*m)[k].as_array();
               for (const auto& iv : given->as_array()) {
                 if (!iv.is_object()) return refuse(std::string("content: each of ") + k + " is an object");
                 std::string path = text(iv.as_object(), "path");
                 auto same = std::find_if(items.begin(), items.end(), [&](const bj::value& x) { return text(x.as_object(), "path") == path; });
                 if (same != items.end()) {
                   *same = iv;
                 } else {
                   items.push_back(iv);
                 }
               }
             }
             if ((*m)["pages"].as_array().size() > 100 || (*m)["files"].as_array().size() > 200) {
               return refuse("content: a tenant's member holds at most 100 pages and 200 files");
             }
             added.push_back(bj::value("content:" + tenant));
           }
         }
         if (auto* a = list("flags")) {
           for (const auto& v : *a) {
             if (!v.is_object()) return refuse("each flag is {tenant, box, flag, value}");
             const auto& o = v.as_object();
             std::string tenant = text(o, "tenant"), box = text(o, "box"), flag = text(o, "flag");
             auto* value = o.if_contains("value");
             if (tenant.empty() || box.empty() || flag.empty() || !value || !value->is_string()) {
               return refuse("each flag is {tenant, box, flag, value}, the value a string");
             }
             std::string val(value->as_string());
             if (val.size() > manifest::Manifest::kFlagValueMax || val.find_first_of("\r\n") != std::string::npos) {
               return refuse("a flag's value is one line of at most 200 bytes");
             }
             put(bj::object{{"kind", "flag"}, {"tenant", tenant}, {"box", box}, {"flag", flag}, {"value", val}});
             added.push_back(bj::value("flag:" + tenant + "/" + box + "/" + flag));
           }
         }
         bj::array removed;
         if (auto* r = args.if_contains("remove"); r && r->is_array()) {
           for (const auto& v : r->as_array()) {
             if (!v.is_string()) continue;
             std::string key(v.as_string());
             auto before = members.size();
             std::erase_if(members, [&](const auto& kv) { return kv.first == key; });
             if (members.size() != before) {
               removed.push_back(bj::value(key));
               continue;
             }
             // content:tenant:path drops one page or file from a content member.
             if (key.starts_with("content:")) {
               auto colon = key.find(':', 8);
               if (colon == std::string::npos) continue;
               if (bj::object* m = find(key.substr(0, colon))) {
                 std::string path = key.substr(colon + 1), bare = path;
                 while (!bare.empty() && bare.front() == '/') bare.erase(bare.begin());
                 bool hit = false;
                 for (const char* k : {"pages", "files"}) {
                   auto& items = (*m)[k].as_array();
                   auto n = items.size();
                   items.erase(std::remove_if(items.begin(), items.end(),
                                              [&](const bj::value& x) {
                                                std::string p = text(x.as_object(), "path");
                                                return p == path || p == bare;
                                              }),
                               items.end());
                   hit |= items.size() != n;
                 }
                 if (hit) removed.push_back(bj::value(key));
               }
             }
           }
         }
         size_t counts[4] = {0, 0, 0, 0};
         for (const auto& [k, m] : members) ++counts[kind_order(text(m, "kind"))];
         if (counts[0] > kMaxSchemas || counts[1] > kMaxVersions || counts[2] > kMaxContent || counts[3] > kMaxFlags) {
           return refuse("a change set holds at most 20 schemas, 20 versions, 10 tenants' content, and 50 flags");
         }
         for (const auto& [k, m] : members) {
           if (auto refused = changeset_scope_refusal(m)) return refuse(k + ": " + *refused);
         }
         // Kept in the order they apply, so the draft reads the way it will run.
         std::stable_sort(members.begin(), members.end(), [](const auto& a, const auto& b) {
           return kind_order(text(a.second, "kind")) < kind_order(text(b.second, "kind"));
         });
         bj::array all, keys;
         for (auto& [k, m] : members) {
           keys.push_back(bj::value(k));
           all.push_back(std::move(m));
         }
         (*cs)["members"] = std::move(all);
         int64_t version = cs->at("version").as_int64() + 1;
         (*cs)["version"] = version;
         (*cs)["updated_ms"] = base::now_unix_ms();
         if (!save_changeset(*cs, &err)) return ToolResult::fail(err);
         return ToolResult::ok(bj::object{{"id", cs->at("id")},
                                          {"draft_version", version},
                                          {"added", std::move(added)},
                                          {"removed", std::move(removed)},
                                          {"members", std::move(keys)},
                                          {"live", false},
                                          {"next", "changeset_preview {id} shows what it would do; changeset_apply {id} does it"}});
       }});

  mcp_->add_tool(
      {"changeset_show",
       "Read tool. One change set: its title, why, who made it, state (draft, applied, failed: an apply that stopped "
       "and was undone, rolled_back, or discarded), its members (content items without their bodies), and for an "
       "applied one each step with what it replaced (before), what it made (after), and the snapshots taken; for a "
       "rolled-back one, what the rollback did.",
       object_schema({{"id", id_prop}}, {"id"}),
       [this](const bj::object& args) {
         std::lock_guard lock(changesets_mu_);
         auto cs = load_changeset(text(args, "id"));
         if (!cs || !changeset_visible(*cs)) return ToolResult::fail("no change set " + text(args, "id"));
         // Bodies and modules stay out of the answer: their sizes and hashes say which.
         for (auto& mv : (*cs)["members"].as_array()) {
           auto& m = mv.as_object();
           for (const char* k : {"pages", "files"}) {
             auto* items = m.if_contains(k);
             if (!items) continue;
             for (auto& iv : items->as_array()) {
               auto& item = iv.as_object();
               for (const char* b : {"body", "text", "base64"}) {
                 if (auto* x = item.if_contains(b); x && x->is_string()) {
                   item[std::string(b) + "_sha256"] = base::sha256_hex(std::string(x->as_string()));
                   item[std::string(b) + "_bytes"] = int64_t(x->as_string().size());
                   item.erase(b);
                 }
               }
             }
           }
           if (auto* u = m.if_contains("upload"); u && u->is_object()) {
             auto& up = u->as_object();
             for (const char* b : {"wat", "wasm_base64"}) {
               if (auto* x = up.if_contains(b); x && x->is_string()) {
                 up[std::string(b) + "_bytes"] = int64_t(x->as_string().size());
                 up.erase(b);
               }
             }
           }
         }
         if (auto* a = cs->if_contains("applied"); a && a->is_object()) {
           for (auto& sv : a->as_object()["steps"].as_array()) {
             auto& before = sv.as_object()["before"].as_object();
             if (auto* files = before.if_contains("files")) {
               for (auto& fv : files->as_array()) {
                 auto& f = fv.as_object();
                 if (auto* b = f.if_contains("base64")) {
                   f["bytes"] = int64_t(base::base64_decode(std::string(b->as_string())).value_or("").size());
                   f.erase("base64");
                 } else {
                   f["absent"] = true;
                 }
               }
             }
           }
         }
         return ToolResult::ok(std::move(*cs));
       }});

  mcp_->add_tool(
      {"changeset_list",
       "Read tool. Change sets, newest first: id, title, state, by, members (their keys), and when each was made, "
       "applied, or rolled back. A focused operator sees their own.",
       object_schema({{"state", string_property("Only this state: draft, applied, failed, rolled_back, or discarded.")}}, {}),
       [this](const bj::object& args) {
         std::string only = text(args, "state");
         std::vector<bj::object> rows;
         std::lock_guard lock(changesets_mu_);
         std::error_code ec;
         for (const auto& e : fs::directory_iterator(changeset_file("0").parent_path(), ec)) {
           auto cs = load_changeset(e.path().stem().string());
           if (!cs || !changeset_visible(*cs)) continue;
           if (!only.empty() && text(*cs, "state") != only) continue;
           bj::array keys;
           for (const auto& mv : cs->at("members").as_array()) keys.push_back(bj::value(member_key(mv.as_object())));
           bj::object row{{"id", cs->at("id")},       {"title", cs->at("title")}, {"state", cs->at("state")},
                          {"by", cs->at("by")},       {"members", std::move(keys)},
                          {"created_at", base::format_utc_ms(cs->at("created_ms").as_int64())}};
           for (const char* k : {"applied", "rolled_back"}) {
             if (auto* a = cs->if_contains(k); a && a->is_object()) {
               row[std::string(k) + "_at"] = base::format_utc_ms(a->as_object().at("at_ms").as_int64());
             }
           }
           row["created_ms"] = cs->at("created_ms");
           rows.push_back(std::move(row));
         }
         std::sort(rows.begin(), rows.end(), [](const bj::object& a, const bj::object& b) {
           return a.at("created_ms").as_int64() > b.at("created_ms").as_int64();
         });
         bj::array out;
         for (auto& r : rows) {
           r.erase("created_ms");
           out.push_back(std::move(r));
           if (out.size() >= 100) break;
         }
         return ToolResult::ok(bj::object{{"changesets", std::move(out)}});
       }});

  mcp_->add_tool(
      {"changeset_preview",
       "Read tool. What a change set would do, with nothing stored: each member's own dry run (schema_check for a "
       "schema; version_upload {dry_run} with the set's schemas, or version_diff against the live version, for a "
       "version, plus its tries through route_try; content_deploy {dry_run} for content, rendered signed out; for a "
       "flag, whether the box as it will be declares it and allows the value, and what it is now). Answers ok, "
       "members, problems, and widens: every widening across the set's versions as sentences, for one review. "
       "changeset_apply refuses a change set whose preview has problems.",
       object_schema({{"id", id_prop}}, {"id"}),
       [this](const bj::object& args) {
         std::optional<bj::object> cs;
         {
           std::lock_guard lock(changesets_mu_);
           cs = load_changeset(text(args, "id"));
         }
         if (!cs || !changeset_visible(*cs)) return ToolResult::fail("no change set " + text(args, "id"));
         return ToolResult::ok(changeset_preview(*cs, true));
       }});

  mcp_->add_tool(
      {"changeset_apply",
       "Changing tool. Makes a draft change set live in one call, in order: schemas stored, versions live (an upload "
       "stored dark first, then version_promote), content deployed (content_deploy, verified signed out), flags set. "
       "First it previews (refused, with nothing done, on any problem), and snapshots every existing database the "
       "set's versions may write (db_restore_to_new brings data back). Before each member it keeps what the member "
       "replaces: the live version, every file's bytes, each flag's value. If any member fails, the ones already "
       "applied are undone in reverse and the set is failed. Answers ok, steps, snapshots, and change_ids. Waits for "
       "approval when your approvals are on; the person approving sees the preview. version pins the draft (the "
       "one changeset_add answered); a draft changed since is refused.",
       object_schema({{"id", id_prop},
                      {"version", mcp::integer_property("The draft version to apply (changeset_add's draft_version); "
                                                        "filled in when the call is made.")}},
                     {"id"}),
       [this](const bj::object& args) {
         std::lock_guard lock(changesets_mu_);
         auto cs = load_changeset(text(args, "id"));
         if (!cs || !changeset_visible(*cs)) return ToolResult::fail("no change set " + text(args, "id"));
         std::string id = text(*cs, "id");
         if (text(*cs, "state") != "draft") return ToolResult::fail("change set " + id + " is " + text(*cs, "state") + ", not a draft");
         if (auto* v = args.if_contains("version"); v && v->is_int64() && v->as_int64() != cs->at("version").as_int64()) {
           return ToolResult::fail("change set " + id + " changed since this apply was asked for (draft " +
                                   std::to_string(v->as_int64()) + ", now " + std::to_string(cs->at("version").as_int64()) +
                                   "): preview it again and apply that");
         }
         auto preview = changeset_preview(*cs, true);
         if (!preview.at("ok").as_bool()) {
           preview["applied"] = false;
           preview["note"] = "nothing was applied: the preview has problems";
           return ToolResult::ok(std::move(preview));
         }
         std::string op = current_operator(), err;
         int64_t now = base::now_unix_ms();
         // Snapshots of every database a version may write, before anything changes.
         bj::array snapshots;
         std::set<std::pair<std::string, std::string>> writable;
         auto cfg = config_.get();
         for (const auto& mv : cs->at("members").as_array()) {
           const auto& m = mv.as_object();
           if (text(m, "kind") != "version") continue;
           auto target = changeset_manifest(*cs, text(m, "box"));
           if (!target) continue;
           for (const auto& d : target->data) {
             if (!d.applies_schema() || !d.may_write()) continue;
             for (const auto& [t, dm] : tenants_of(*cfg, *target)) writable.insert({d.tenant_for(t), d.database_for(t)});
             if (!d.tenant.empty()) writable.insert({d.tenant, d.database_for(d.tenant)});
           }
         }
         for (const auto& [t, db] : writable) {
           if (!databases_->exists(t, db)) continue;
           auto snap = take_snapshot(t, db, "before change set " + id + " (" + text(*cs, "title") + ")", op, false, &err);
           if (!snap) return ToolResult::fail("no snapshot of " + t + "/" + db + ", so nothing was applied: " + err);
           snapshots.push_back(bj::object{{"tenant", t}, {"database", db}, {"snapshot", snap->at("snapshot")}});
         }
         auto record = [&](const std::string& tool, const bj::object& named, bool failed) {
           if (!changes_) return;
           try {
             bj::object a = named;
             a["changeset"] = id;
             changes_->record_action(operations::Action{base::now_unix_ms(), op, tool, current_request_id(), std::move(a), failed});
           } catch (const std::exception&) {
           }
         };
         auto run = [&](const std::string& tool, const bj::object& a, bj::value* value) -> std::string {
           auto r = mcp_->call(tool, a);
           std::string e = !r ? "no tool " + tool
                           : r->is_error ? (r->value.is_string() ? std::string(r->value.as_string()) : json::dump(r->value))
                                         : "";
           bj::object named;
           for (const char* k : {"tenant", "box", "flag", "version"}) {
             if (auto* v = a.if_contains(k); v && v->is_string()) named[k] = *v;
           }
           record(tool, named, !e.empty());
           if (r && value) *value = r->value;
           return e;
         };
         bj::array steps;
         std::string failed_key, failure;
         for (const auto& mv : cs->at("members").as_array()) {
           const auto& m = mv.as_object();
           std::string kind = text(m, "kind"), key = member_key(m);
           bj::object step{{"kind", kind}, {"key", key}};
           bj::value out;
           if (kind == "schema") {
             step["name"] = m.at("name");
             step["before"] = bj::object{{"stored", schemas_->get(text(m, "name")).has_value()}};
             failure = run("schema_put", bj::object{{"schema", m.at("schema")}}, &out);
           } else if (kind == "version") {
             std::string box = text(m, "box");
             step["box"] = box;
             auto active = boxes_->active();
             bj::object before;
             if (auto live = active->boxes.find(box); live != active->boxes.end()) {
               before["active"] = live->second->version;
             } else if (auto d = boxes_->disabled(box)) {
               before["disabled"] = d->version;
               before["why"] = d->why;
             }
             step["before"] = before;
             std::string version = text(m, "version");
             if (auto* u = m.if_contains("upload"); u && u->is_object()) {
               bj::object a = u->as_object();
               a["activate"] = false;
               failure = run("version_upload", a, &out);
               if (failure.empty()) version = text(out.as_object(), "version");
             }
             if (failure.empty()) failure = run("version_promote", bj::object{{"box", box}, {"version", version}}, &out);
             step["after"] = bj::object{{"active", version}};
           } else if (kind == "content") {
             std::string tenant = text(m, "tenant");
             step["tenant"] = tenant;
             auto files = content_deploy_files(tenant, m, &err).value_or(std::vector<std::string>{});
             bj::array kept;
             for (const auto& f : files) {
               bj::object row{{"file", f}};
               if (auto got = content_->get(tenant, f)) row["base64"] = base::base64_encode(got->data);
               kept.push_back(std::move(row));
             }
             step["before"] = bj::object{{"files", std::move(kept)}};
             bj::object a{{"tenant", tenant}};
             for (const char* k : {"pages", "files"}) {
               if (auto* v = m.if_contains(k); v && v->is_array() && !v->as_array().empty()) a[k] = *v;
             }
             failure = run("content_deploy", a, &out);
             if (failure.empty()) {
               const auto& r = out.as_object();
               step["lines"] = r.at("lines");
               if (auto* o = r.if_contains("ok"); !o || !o->is_bool() || !o->as_bool()) {
                 failure = "content_deploy found problems (its lines say which)";
               }
             }
             bj::object etags;
             for (const auto& f : files) {
               auto got = content_->get(tenant, f);
               etags[f] = got ? bj::value(got->etag) : bj::value(nullptr);
             }
             step["after"] = bj::object{{"file_etags", std::move(etags)}};
           } else {
             std::string tenant = text(m, "tenant"), box = text(m, "box"), name = text(m, "flag");
             step["tenant"] = tenant;
             step["box"] = box;
             step["flag"] = name;
             auto active = boxes_->active();
             std::string before;
             if (auto live = active->boxes.find(box); live != active->boxes.end()) {
               before = flag_for(live->second->manifest, tenant, name).value_or("");
             }
             step["before"] = bj::object{{"value", before}};
             failure = run("flag_set", bj::object{{"tenant", tenant}, {"box", box}, {"flag", name}, {"value", m.at("value")},
                                                  {"reason", "change set " + id + ": " + text(*cs, "title")}},
                           &out);
             step["after"] = bj::object{{"value", m.at("value")}};
           }
           if (!failure.empty()) {
             step["error"] = failure;
             failed_key = key;
             // A member that failed partway (content written, then a check failed) is undone too.
             if (kind == "content") steps.push_back(std::move(step));
             break;
           }
           steps.push_back(std::move(step));
         }
         bj::object applied{{"at_ms", now}, {"by", op}, {"steps", steps}, {"snapshots", snapshots}};
         if (!failed_key.empty()) {
           bj::array undone;
           for (auto it = steps.rbegin(); it != steps.rend(); ++it) undone.push_back(changeset_undo(it->as_object(), id));
           applied["failed"] = bj::object{{"member", failed_key}, {"error", failure}};
           applied["undone"] = undone;
           (*cs)["state"] = "failed";
           (*cs)["applied"] = applied;
           save_changeset(*cs, &err);
           audit("changeset.apply", op, "changeset:" + id, "failed", bj::object{{"member", failed_key}, {"error", failure}});
           return ToolResult::ok(bj::object{{"id", id},
                                            {"ok", false},
                                            {"failed", failed_key},
                                            {"error", failure},
                                            {"undone", std::move(undone)},
                                            {"snapshots", std::move(snapshots)},
                                            {"state", "failed"},
                                            {"note", "the members applied before it were undone in reverse; schemas stay stored"}});
         }
         (*cs)["state"] = "applied";
         (*cs)["applied"] = applied;
         if (!save_changeset(*cs, &err)) return ToolResult::fail("applied, but couldn't keep the record: " + err);
         audit("changeset.apply", op, "changeset:" + id, "ok", bj::object{{"members", int64_t(steps.size())}});
         return ToolResult::ok(bj::object{{"id", id},
                                          {"ok", true},
                                          {"state", "applied"},
                                          {"steps", std::move(steps)},
                                          {"snapshots", std::move(snapshots)},
                                          {"widens", preview.at("widens")},
                                          {"next", "changeset_rollback {id} undoes all of it; version_health watches each box"}});
       }});

  mcp_->add_tool(
      {"changeset_rollback",
       "Changing tool. Undoes an applied change set in one call, in reverse: flags set back to what they were, "
       "content files put back byte for byte (or removed if the set made them), each box back to the version it had "
       "(or disabled again, as it was), schemas left stored (they only grow; columns a version added stay, unused). "
       "Data written since isn't touched: the snapshots apply took are named, for db_restore_to_new. Refused when "
       "something changed since apply (a newer version promoted, a file edited, a flag set) unless force: true, and "
       "the answer names each. Waits for approval when your approvals are on.",
       object_schema({{"id", id_prop},
                      {"force", boolean_property("Roll back over changes made since apply (they are lost).")}},
                     {"id"}),
       [this](const bj::object& args) {
         std::lock_guard lock(changesets_mu_);
         auto cs = load_changeset(text(args, "id"));
         if (!cs || !changeset_visible(*cs)) return ToolResult::fail("no change set " + text(args, "id"));
         std::string id = text(*cs, "id");
         if (text(*cs, "state") != "applied") {
           return ToolResult::fail("change set " + id + " is " + text(*cs, "state") + ": only an applied one rolls back");
         }
         for (const auto& mv : cs->at("members").as_array()) {
           if (auto refused = changeset_scope_refusal(mv.as_object())) return ToolResult::fail(*refused);
         }
         const auto& steps = cs->at("applied").at("steps").as_array();
         // What changed since apply, so a rollback doesn't silently undo a teammate's work.
         bj::array drift;
         auto active = boxes_->active();
         for (const auto& sv : steps) {
           const auto& s = sv.as_object();
           std::string kind = text(s, "kind");
           if (kind == "version") {
             auto live = active->boxes.find(text(s, "box"));
             std::string want = text(s.at("after").as_object(), "active");
             if (live == active->boxes.end() || live->second->version != want) {
               drift.push_back(bj::value("box " + text(s, "box") + " isn't on " + want + " any more"));
             }
           } else if (kind == "content") {
             for (const auto& [f, etag] : s.at("after").at("file_etags").as_object()) {
               auto got = content_->get(text(s, "tenant"), f);
               bool same = etag.is_null() ? !got : (got && got->etag == etag.as_string());
               if (!same) drift.push_back(bj::value(text(s, "tenant") + " " + std::string(f) + " changed since"));
             }
           } else if (kind == "flag") {
             auto live = active->boxes.find(text(s, "box"));
             std::string now = live != active->boxes.end()
                                   ? flag_for(live->second->manifest, text(s, "tenant"), text(s, "flag")).value_or("")
                                   : "";
             if (now != text(s.at("after").as_object(), "value")) {
               drift.push_back(bj::value("flag " + text(s, "flag") + " of " + text(s, "box") + " is " + now + " now"));
             }
           }
         }
         if (!drift.empty() && !flag_of(args, "force")) {
           return ToolResult::fail("changed since change set " + id + " was applied: " + json::dump(drift) +
                                   "; nothing was rolled back (force: true rolls back over them)");
         }
         bj::array undone;
         bool ok = true;
         for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
           auto u = changeset_undo(it->as_object(), id);
           ok &= !u.contains("error");
           undone.push_back(std::move(u));
         }
         std::string op = current_operator(), err;
         (*cs)["state"] = ok ? "rolled_back" : "applied";
         (*cs)["rolled_back"] = bj::object{{"at_ms", base::now_unix_ms()}, {"by", op}, {"undone", undone}, {"ok", ok},
                                            {"forced_over", drift}};
         save_changeset(*cs, &err);
         audit("changeset.rollback", op, "changeset:" + id, ok ? "ok" : "failed", bj::object{{"forced_over", drift}});
         if (changes_) {
           try {
             changes_->record_action(operations::Action{base::now_unix_ms(), op, "changeset_rollback", current_request_id(),
                                                        bj::object{{"changeset", id}}, !ok});
           } catch (const std::exception&) {
           }
         }
         return ToolResult::ok(bj::object{{"id", id},
                                          {"ok", ok},
                                          {"state", ok ? "rolled_back" : "applied"},
                                          {"undone", std::move(undone)},
                                          {"snapshots", cs->at("applied").at("snapshots")},
                                          {"note", ok ? "data written since apply stays; db_restore_to_new {snapshot} brings a "
                                                        "database back as it was before"
                                                      : "some members couldn't be undone (their error says why); the set "
                                                        "stays applied, so you can fix that and roll back again"}});
       }});

  mcp_->add_tool(
      {"changeset_discard",
       "Drops a draft change set: it is kept, marked discarded, and can't be applied. An applied one is undone with "
       "changeset_rollback instead. Changes only the draft, so it never waits for approval.",
       object_schema({{"id", id_prop}}, {"id"}),
       [this](const bj::object& args) {
         std::lock_guard lock(changesets_mu_);
         auto cs = load_changeset(text(args, "id"));
         if (!cs || !changeset_visible(*cs)) return ToolResult::fail("no change set " + text(args, "id"));
         if (text(*cs, "state") != "draft") return ToolResult::fail("only a draft is discarded; this one is " + text(*cs, "state"));
         (*cs)["state"] = "discarded";
         (*cs)["updated_ms"] = base::now_unix_ms();
         std::string err;
         if (!save_changeset(*cs, &err)) return ToolResult::fail(err);
         return ToolResult::ok(bj::object{{"id", cs->at("id")}, {"state", "discarded"}});
       }});
}

std::optional<bj::object> Host::changeset_for_approval(const std::string& id) {
  std::optional<bj::object> cs;
  {
    std::lock_guard lock(changesets_mu_);
    cs = load_changeset(id);
  }
  if (!cs) return std::nullopt;
  // No tries here: whoever looks at an approval only looks.
  return changeset_preview(*cs, false);
}

std::optional<int64_t> Host::changeset_draft_version(const std::string& id) {
  std::lock_guard lock(changesets_mu_);
  auto cs = load_changeset(id);
  if (!cs) return std::nullopt;
  return cs->at("version").as_int64();
}

}  // namespace goober::host
