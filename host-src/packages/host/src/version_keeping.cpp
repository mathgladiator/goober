// Keeping box versions (design/machine/045.Limits.md § 3.1): a version no longer live is
// archived after limits.version-archive-days and deleted after limits.version-delete-days,
// leaving a tombstone; never the live one, a disabled box's, the newest limits.version-keep that
// were live, or one a kept change set may roll back to. version_restore brings an archived one
// back.

#include <functional>
#include <map>

#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/source/source.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {

constexpr int64_t kDayMs = 86400000;

std::optional<std::string> text_of(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

// Every version of `box` an object names: {"box": box, and "version", "active", or "disabled"}.
void versions_named(const bj::value& v, const std::string& box, std::set<std::string>* out) {
  if (const auto* o = v.if_object()) {
    const auto* b = o->if_contains("box");
    if (b && b->is_string() && b->as_string() == box) {
      for (const char* k : {"version", "active", "disabled"}) {
        if (const auto* x = o->if_contains(k); x && x->is_string()) out->insert(std::string(x->as_string()));
      }
    }
    for (const auto& kv : *o) versions_named(kv.value(), box, out);
  } else if (const auto* a = v.if_array()) {
    for (const auto& x : *a) versions_named(x, box, out);
  }
}

}  // namespace

std::map<std::string, std::string> Host::changeset_versions(const std::string& box) {
  std::map<std::string, std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(changeset_file("0").parent_path(), ec)) {
    if (e.path().extension() != ".json") continue;
    auto cs = load_changeset(e.path().stem().string());
    if (!cs) continue;
    std::set<std::string> named;
    versions_named(*cs, box, &named);
    for (const auto& v : named) {
      out.emplace(v, "named by change set " + e.path().stem().string() + ", kept until it is deleted (042 § 9)");
    }
  }
  return out;
}

void Host::setup_version_keeping() {
  auto cfg = config_.get();
  boxes_->set_retention(boxes::Retention{cfg->disk.version_archive_days * kDayMs, cfg->disk.version_delete_days * kDayMs,
                                         cfg->disk.version_keep});
  boxes_->set_protector([this](const std::string& box) { return changeset_versions(box); });
  boxes_->set_actor([] { return current_operator(); });
}

boxes::PruneReport Host::prune_versions(int64_t now_ms) {
  auto report = boxes_->prune(now_ms, [this](const std::string& box, const std::string& version) {
    bj::object more;
    if (source_) {
      if (auto d = source_->deploy_of_version(box, version)) {
        more["source"] = bj::object{{"project", d->project}, {"rev", d->rev}, {"job", d->job}};
      }
    }
    return more;
  });
  for (const auto& [box, version] : report.archived) {
    audit("version.archive", "host", "box:" + box, "ok", bj::object{{"version", version}});
  }
  for (const auto& t : report.deleted) {
    bj::object more{{"version", t.at("version")}, {"module_sha256", t.at("module_sha256")}};
    audit("version.delete", "host", "box:" + std::string(t.at("box").as_string()), "ok", std::move(more));
  }
  return report;
}

void Host::register_version_keeping_tools() {
  mcp_->add_tool(
      {"version_restore",
       "Changing tool. Brings an archived box version back to stored, so route_try, job_dry_run, and "
       "version_promote take it again. A version is archived 30 days after it stopped being live (or was stored "
       "dark and never promoted), and deleted 400 days after, keeping only a tombstone (045 § 3.1); restoring "
       "starts its 30 days again. version_list says each version's state and when it will be archived or "
       "deleted. A deleted version can't be restored: upload it again. Audited.",
       mcp::object_schema({{"box", mcp::string_property("The box name.")},
                           {"version", mcp::string_property("The archived version.")},
                           {"why", mcp::string_property("Why it's needed again, in a sentence.")}},
                          {"box", "version", "why"}),
       [this](const bj::object& args) {
         auto box = text_of(args, "box");
         auto version = text_of(args, "version");
         auto why = text_of(args, "why").value_or("");
         if (!box || !version) return ToolResult::fail("box and version are required");
         if (why.empty()) return ToolResult::fail("why is required: say why the version is needed again");
         std::string err;
         bool ok = boxes_->restore(*box, *version, &err);
         audit("version.restore", current_operator(), "box:" + *box, ok ? "ok" : "error",
               bj::object{{"version", *version}, {"why", why.substr(0, 500)}, {"error", ok ? "" : err}});
         if (!ok) return ToolResult::fail(err);
         int64_t archive_at = base::now_unix_ms() + config_.get()->disk.version_archive_days * kDayMs;
         return ToolResult::ok(bj::object{{"box", *box},
                                          {"version", *version},
                                          {"state", "stored"},
                                          {"archives_at", base::format_utc_ms(archive_at)},
                                          {"next", "route_try or version_promote {box, version} take it now"}});
       }});
}

}  // namespace goober::host
