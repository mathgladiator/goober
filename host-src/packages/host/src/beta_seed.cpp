// Seeding beta from production (design/machine/053.Beta.md § 13): a site's databases, and its
// content when asked, copied over its beta tenant's, so beta is tried against real data. Beta's
// own databases are snapshotted first. Accounts, keys, flags, and attachments stay apart. Only an
// operator who isn't focused seeds: beta's operator never pulls production's data itself.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

bool flag(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_bool() && v->as_bool();
}

// Files the host keeps itself, never carried.
bool host_kept(const std::string& path) { return path.find("_versions/") != std::string::npos; }

}  // namespace

std::optional<bj::object> Host::beta_seed(const std::string& tenant, const bj::object& args, std::string* error) {
  auto cfg = config_.get();
  if (cfg->is_beta_tenant(tenant)) return *error = tenant + " is a beta tenant: name the production tenant it is the beta of", std::nullopt;
  std::string beta = cfg->beta_tenant_of(tenant);
  if (beta.empty()) return *error = tenant + " has no beta site (beta-of in host.json)", std::nullopt;
  bool dry_run = flag(args, "dry_run"), with_content = flag(args, "content");
  // Which databases: those named, else every one production has.
  std::vector<std::string> names;
  if (const auto* d = args.if_contains("databases"); d && d->is_array()) {
    for (const auto& v : d->as_array()) {
      if (!v.is_string()) return *error = "databases is a list of names", std::nullopt;
      std::string n(v.as_string());
      if (!databases_->exists(tenant, n)) return *error = "tenant " + tenant + " has no database " + n, std::nullopt;
      names.push_back(n);
    }
  } else {
    names = databases_->databases(tenant);
  }
  bj::array dbs;
  int64_t total = 0;
  for (const auto& n : names) {
    std::error_code ec;
    int64_t bytes = int64_t(fs::file_size(databases_->file(tenant, n), ec));
    total += bytes;
    bool replaces = databases_->exists(beta, n);
    dbs.push_back(bj::object{{"database", n}, {"bytes", bytes}, {"replaces_beta", replaces}});
  }
  // Content: production's files beta lacks or has otherwise; beta's own other files stay.
  std::vector<std::string> paths;
  if (with_content) {
    for (const auto& path : content_->list(tenant, "")) {
      if (host_kept(path)) continue;
      auto pf = content_->get(tenant, path);
      auto bf = content_->get(beta, path);
      if (pf && (!bf || bf->data != pf->data)) paths.push_back(path);
    }
  }
  bj::array content;
  for (const auto& p : paths) content.push_back(bj::value(p));
  bj::object out{{"tenant", tenant},
                 {"beta_tenant", beta},
                 {"databases", std::move(dbs)},
                 {"content", std::move(content)},
                 {"not_carried", "accounts (rows naming production's user ids name no one in beta), keys, flags, and "
                                 "attachments; beta's other content files stay"},
                 {"note", "beta's operators (an agent held to beta) read what is seeded: production's data, copied"}};
  if (dry_run) {
    out["dry_run"] = true;
    return out;
  }
  if (names.empty() && paths.empty()) return *error = "nothing to seed: " + tenant + " has no databases" + (with_content ? " and beta's content matches" : ""), std::nullopt;
  if (auto refused = free_space_refused(total, "seeding " + beta + " from " + tenant); !refused.empty()) return *error = refused, std::nullopt;
  std::string by = current_operator(), why = text_of(args, "why");
  // Beta's own first, every one, before anything is copied: a refusal leaves beta as it was.
  bj::object snapshots;
  for (const auto& n : names) {
    if (!databases_->exists(beta, n)) continue;
    std::string err;
    auto snap = take_snapshot(beta, n, "before seeding from " + tenant + (why.empty() ? "" : ": " + why), by, false, &err);
    if (!snap) return *error = "beta's " + n + " couldn't be snapshotted, so nothing was seeded: " + err, std::nullopt;
    snapshots[n] = snap->at("snapshot");
  }
  bj::array seeded;
  for (const auto& n : names) {
    std::string err;
    if (!databases_->copy_from(tenant, beta, n, &err)) {
      out["seeded"] = std::move(seeded);
      out["snapshots"] = std::move(snapshots);
      return *error = "seeding stopped at " + n + " (" + err + "); seeded before it: " + json::dump(out.at("seeded")) +
                      "; beta's snapshots: " + json::dump(out.at("snapshots")),
             std::nullopt;
    }
    seeded.push_back(bj::value(n));
  }
  int64_t files = 0;
  {
    std::lock_guard writes(content_writes_mu_);
    for (const auto& p : paths) {
      auto f = content_->get(tenant, p);
      std::string err;
      if (f && content_->put(beta, p, f->data, &err)) ++files;
      else if (f) std::cerr << "goober-host: beta seed: " << p << ": " << err << "\n";
    }
  }
  bj::object detail{{"tenant", tenant}, {"beta_tenant", beta}, {"databases", seeded}, {"snapshots", snapshots},
                    {"content_files", files}, {"why", why.substr(0, 500)}};
  if (changes_) {
    try {
      changes_->record_action(operations::Action{base::now_unix_ms(), by, "beta_seed", current_request_id(), detail, false});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  }
  audit("beta.seed", by, beta, "ok", detail);
  out["seeded"] = std::move(seeded);
  out["snapshots"] = std::move(snapshots);
  out["content_files"] = files;
  return out;
}

void Host::register_beta_seed_tools() {
  using mcp::object_schema;
  using mcp::string_property;
  mcp_->add_tool(
      {"beta_seed",
       "Changing tool. Seeds a site's beta from production: production's databases (all, or those named) copied "
       "over the beta tenant's, each of beta's own snapshotted first (db_snapshots on the beta tenant; a snapshot "
       "refused by its limits stops the seed before anything is copied); with content: true, production's content "
       "files that beta lacks or has otherwise too (beta's other files stay). Never accounts, keys, flags, or "
       "attachments. Beta's operators then read a copy of production's data. dry_run answers what it would copy. "
       "Only an operator not focused on tenants seeds. In the change log. design/machine/053.Beta.md § 13.",
       object_schema({{"tenant", string_property("The production tenant, like acme.")},
                      {"databases", bj::object{{"type", "array"}, {"items", bj::object{{"type", "string"}}},
                                               {"description", "Only these databases; every one production has when left out."}}},
                      {"content", mcp::boolean_property("Copy production's content files over beta's too.")},
                      {"dry_run", mcp::boolean_property("Answer what it would copy, and change nothing.")},
                      {"why", string_property("Why, for the change log.")}},
                     {"tenant"}),
       [this](const bj::object& args) {
         std::string err;
         auto out = beta_seed(text_of(args, "tenant"), args, &err);
         if (!out) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*out));
       }});
}

}  // namespace goober::host
