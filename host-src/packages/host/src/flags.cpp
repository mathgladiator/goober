// Box flags (design/machine/039.Flags.md): declared in the manifest, set by operators, read by
// boxes through goober_flag.

#include "flags.h"

#include <algorithm>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

std::string file_of(const std::string& tenant, const std::string& box) { return "flags/" + tenant + "/" + box + ".json"; }

}  // namespace

bj::object& FlagStore::load(const std::string& tenant, const std::string& box) {
  std::string key = tenant + "/" + box;
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second;
  bj::object values;
  if (auto text = base::read_file(wal_.root() / file_of(tenant, box))) {
    std::string err;
    if (auto v = json::parse(*text, &err); v && v->is_object()) values = v->as_object();
  }
  return cache_[key] = std::move(values);
}

bj::object FlagStore::of(const std::string& tenant, const std::string& box) {
  std::lock_guard lock(mu_);
  return load(tenant, box);
}

std::optional<std::string> FlagStore::value(const std::string& tenant, const std::string& box, const std::string& flag) {
  std::lock_guard lock(mu_);
  const auto& values = load(tenant, box);
  const auto* entry = values.if_contains(flag);
  if (!entry || !entry->is_object()) return std::nullopt;
  const auto* v = entry->as_object().if_contains("value");
  if (!v || !v->is_string()) return std::nullopt;
  return std::string(v->as_string());
}

bool FlagStore::set(const std::string& tenant, const std::string& box, const std::string& flag, const std::string& value,
                    const std::string& by, const std::string& why, int64_t now_ms, std::string* error) {
  std::lock_guard lock(mu_);
  bj::object next = load(tenant, box);
  next[flag] = bj::object{{"value", value}, {"by", by}, {"why", why}, {"at", base::format_utc_ms(now_ms)}};
  if (!wal_.commit({{wal::FileChange::Kind::kWrite, file_of(tenant, box), json::pretty(next)}}, error)) return false;
  cache_[tenant + "/" + box] = std::move(next);
  return true;
}

std::optional<std::string> Host::flag_for(const manifest::Manifest& m, const std::string& tenant, std::string_view name) {
  const auto* f = m.flag(name);
  if (!f) return std::nullopt;
  auto set = flags_ ? flags_->value(tenant, m.box, f->name) : std::nullopt;
  // A value the manifest no longer allows reads as the default, as an unset one does.
  if (!set || (!f->values.empty() && std::find(f->values.begin(), f->values.end(), *set) == f->values.end())) {
    return f->default_value;
  }
  return set;
}

void Host::register_flag_tools() {
  flags_ = std::make_unique<FlagStore>(*wal_);
  auto tenant_prop = mcp::string_property("The tenant the flag is set for, like example.");
  auto box_prop = mcp::string_property("The box, as box_list names it.");

  // Whether an active box serves a tenant (its domains, or a database pinned to it).
  auto serves = [this](const manifest::Manifest& m, const std::string& tenant) {
    bool yes = false;
    for (const auto& [tn, dm] : tenants_of(*config_.get(), m)) yes |= tn == tenant;
    for (const auto& d : m.data) yes |= d.applies_schema() && d.tenant == tenant;
    return yes;
  };

  mcp_->add_tool(
      {"flag_set",
       "Sets one of a box's flags for a tenant: a switch the box's manifest declares under \"flags\" and the box "
       "reads with goober_flag on its next call, with no new version (a freeze, a kill switch, a mode). The value is "
       "a string, one of the flag's declared values when it has them. Audited with the reason, and shown in the "
       "change log with what it was before. Not for per-person state: that belongs in the box's database.",
       mcp::object_schema({{"tenant", tenant_prop},
                           {"box", box_prop},
                           {"flag", mcp::string_property("The flag's name, as the manifest declares it.")},
                           {"value", mcp::string_property("The new value: a string, at most 200 bytes.")},
                           {"reason", mcp::string_property("Why, in a sentence: kept with the flag and in the audit log.")}},
                          {"tenant", "box", "flag", "value", "reason"}),
       [this, serves](const bj::object& args) {
         auto tenant = arg(args, "tenant").value_or("");
         auto box_name = arg(args, "box").value_or("");
         auto flag = arg(args, "flag").value_or("");
         auto value = arg(args, "value");
         auto reason = arg(args, "reason").value_or("");
         if (!value) return ToolResult::fail("value is required, as a string (like \"true\")");
         if (reason.empty()) return ToolResult::fail("reason is required: say why the flag changes");
         auto active = boxes_->active();
         auto it = active->boxes.find(box_name);
         if (it == active->boxes.end()) return ToolResult::fail("no active box named " + box_name);
         const auto& m = it->second->manifest;
         const auto* f = m.flag(flag);
         if (!f) {
           std::string names;
           for (const auto& d : m.flags) names += (names.empty() ? "" : ", ") + d.name;
           return ToolResult::fail("box " + box_name + " declares no flag " + flag +
                                   (names.empty() ? " (it declares none)" : " (it declares: " + names + ")"));
         }
         if (!serves(m, tenant)) return ToolResult::fail("box " + box_name + " doesn't serve tenant " + tenant);
         if (value->size() > manifest::Manifest::kFlagValueMax || value->find_first_of("\r\n") != std::string::npos) {
           return ToolResult::fail("a flag's value is one line of at most 200 bytes");
         }
         if (!f->values.empty() && std::find(f->values.begin(), f->values.end(), *value) == f->values.end()) {
           std::string allowed;
           for (const auto& v : f->values) allowed += (allowed.empty() ? "" : ", ") + v;
           return ToolResult::fail(flag + " takes one of: " + allowed);
         }
         std::string before = *flag_for(m, tenant, flag);
         std::string by = current_operator(), err;
         if (!flags_->set(tenant, box_name, flag, *value, by, reason, base::now_unix_ms(), &err)) {
           return ToolResult::fail("couldn't keep the flag: " + err);
         }
         audit("flag.set", by, "flag:" + tenant + "/" + box_name + "/" + flag, "ok",
               bj::object{{"tenant", tenant}, {"value", *value}, {"before", before}, {"why", reason}});
         return ToolResult::ok(bj::object{{"tenant", tenant}, {"box", box_name}, {"flag", flag}, {"value", *value},
                                          {"before", before}, {"note", "the box reads it on its next call"}});
       }});

  mcp_->add_tool(
      {"flag_list",
       "A tenant's box flags: for each active box serving it (or the one given), every flag its manifest declares, "
       "with the value the box reads now, the default, the allowed values, and who set it, when, and why. A flag set "
       "but no longer declared shows under retired.",
       mcp::object_schema({{"tenant", tenant_prop}, {"box", box_prop}}, {"tenant"}),
       [this, serves](const bj::object& args) {
         auto tenant = arg(args, "tenant").value_or("");
         auto only = arg(args, "box");
         bj::array boxes;
         for (const auto& [name, box] : boxes_->active()->boxes) {
           if (only && *only != name) continue;
           const auto& m = box->manifest;
           if (m.flags.empty() || !serves(m, tenant)) continue;
           auto stored = flags_->of(tenant, name);
           bj::array flags, retired;
           for (const auto& f : m.flags) {
             bj::object o{{"flag", f.name}, {"value", *flag_for(m, tenant, f.name)}, {"default", f.default_value}};
             if (!f.values.empty()) {
               bj::array vs;
               for (const auto& v : f.values) vs.emplace_back(v);
               o["values"] = std::move(vs);
             }
             if (!f.about.empty()) o["about"] = f.about;
             if (const auto* s = stored.if_contains(f.name); s && s->is_object()) {
               for (const char* k : {"by", "why", "at"}) {
                 if (const auto* v = s->as_object().if_contains(k)) o[std::string("set_") + k] = *v;
               }
             }
             flags.push_back(std::move(o));
           }
           for (const auto& kv : stored) {
             if (!m.flag(kv.key())) retired.push_back(bj::object{{"flag", kv.key()}, {"last", kv.value()}});
           }
           bj::object row{{"box", name}, {"version", box->version}, {"flags", std::move(flags)}};
           if (!retired.empty()) row["retired"] = std::move(retired);
           boxes.push_back(std::move(row));
         }
         return ToolResult::ok(bj::object{{"tenant", tenant}, {"boxes", std::move(boxes)}});
       }});
}

}  // namespace goober::host
