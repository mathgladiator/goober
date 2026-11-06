// Promotion (design/machine/053.Beta.md § 11): what beta changed, carried to production as one
// change set (042), so it is previewed together, applied in order, and rolled back by one call.
// Boxes whose beta version differs from the live one, and content files that differ between the
// beta tenant and the production tenant; never data, accounts, keys, flags, or attachments.

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/host/host.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

constexpr size_t kTextShown = 64 * 1024;  // a file's before and after, shown when both are text and this small

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

bool is_text(const std::string& s) { return s.find('\0') == std::string::npos && base::valid_utf8(s) == s; }

// Files a promotion never carries: kept copies the host makes itself.
bool host_kept(const std::string& path) { return path.find("_versions/") != std::string::npos; }

std::set<std::string> names_of(const bj::object& args, const char* k, bool* given) {
  std::set<std::string> out;
  const auto* v = args.if_contains(k);
  *given = v && v->is_array();
  if (*given) {
    for (const auto& x : v->as_array()) {
      if (x.is_string()) out.insert(std::string(x.as_string()));
    }
  }
  return out;
}

}  // namespace

std::optional<bj::object> Host::promotion_plan(const std::string& tenant, std::string* error) {
  auto cfg = config_.get();
  std::string beta = cfg->beta_tenant_of(tenant);
  if (beta.empty()) return *error = tenant + " has no beta site (beta-of in host.json)", std::nullopt;
  auto active = boxes_->active();
  // Boxes: each beta version that differs from the live one, for a box serving this tenant.
  bj::array boxes;
  for (const auto& [name, b] : active->beta) {
    bool serves = false;
    for (const auto& [d, e] : cfg->domains) serves |= e.tenant == tenant && b->manifest.applies_to(d);
    if (!serves) continue;
    auto live = active->boxes.find(name);
    std::string from = live == active->boxes.end() ? "" : live->second->version;
    if (from == b->version) continue;
    bj::object row{{"box", name}, {"from", from.empty() ? bj::value(nullptr) : bj::value(from)}, {"to", b->version}};
    if (from.empty()) {
      row["note"] = "a new box: it goes live in production";
    } else if (auto r = mcp_->call("version_diff", bj::object{{"box", name}, {"from", from}, {"to", b->version}}); r && !r->is_error) {
      if (const auto* w = r->value.is_object() ? r->value.as_object().if_contains("widens") : nullptr) row["widens"] = *w;
      row["review"] = r->value;
    }
    boxes.push_back(std::move(row));
  }
  // Content: every file of beta's that production lacks or has otherwise.
  bj::array content, only_production;
  bj::array warnings;
  std::set<std::string> beta_paths;
  for (const auto& path : content_->list(beta, "")) {
    if (host_kept(path)) continue;
    beta_paths.insert(path);
    auto bf = content_->get(beta, path);
    if (!bf) continue;
    auto pf = content_->get(tenant, path);
    if (pf && pf->data == bf->data) continue;
    bj::object row{{"path", path}, {"change", pf ? "changed" : "new"}, {"bytes_after", int64_t(bf->data.size())}};
    if (pf) row["bytes_before"] = int64_t(pf->data.size());
    if (is_text(bf->data) && bf->data.size() <= kTextShown && (!pf || (is_text(pf->data) && pf->data.size() <= kTextShown))) {
      row["after"] = bf->data;
      if (pf) row["before"] = pf->data;
    }
    if (bf->data.find("/attachment/") != std::string::npos) {
      warnings.push_back(bj::value(path + " links an attachment by id: beta's attachments aren't carried, so put it in production first"));
    }
    content.push_back(std::move(row));
  }
  for (const auto& path : content_->list(tenant, "")) {
    if (!host_kept(path) && !beta_paths.count(path)) only_production.push_back(bj::value(path));
  }
  return bj::object{{"tenant", tenant},
                    {"beta_tenant", beta},
                    {"boxes", std::move(boxes)},
                    {"content", std::move(content)},
                    {"only_in_production", std::move(only_production)},
                    {"warnings", std::move(warnings)},
                    {"not_carried", "data (each tenant keeps its own databases), accounts, keys, flags, and attachments"}};
}

std::optional<bj::object> Host::promotion_start(const std::string& tenant, const bj::object& args, std::string* error) {
  auto plan = promotion_plan(tenant, error);
  if (!plan) return std::nullopt;
  bool boxes_given = false, paths_given = false;
  auto only_boxes = names_of(args, "boxes", &boxes_given);
  auto only_paths = names_of(args, "paths", &paths_given);
  bj::array versions, files;
  for (const auto& b : plan->at("boxes").as_array()) {
    std::string box = text_of(b.as_object(), "box");
    if (!boxes_given || only_boxes.count(box)) versions.push_back(bj::object{{"box", box}, {"version", b.at("to")}});
  }
  std::string beta = text_of(*plan, "beta_tenant");
  for (const auto& c : plan->at("content").as_array()) {
    std::string path = text_of(c.as_object(), "path");
    if (paths_given && !only_paths.count(path)) continue;
    auto f = content_->get(beta, path);
    if (!f) continue;
    if (is_text(f->data)) {
      files.push_back(bj::object{{"path", path}, {"text", f->data}});
    } else {
      files.push_back(bj::object{{"path", path}, {"base64", base::base64_encode(f->data)}});
    }
  }
  if (versions.empty() && files.empty()) return *error = "nothing to promote: beta and " + tenant + " are the same, or nothing was chosen", std::nullopt;
  std::string why = text_of(args, "why");
  auto created = mcp_->call("changeset_create", bj::object{{"title", "promote " + beta + " to " + tenant}, {"why", why}});
  if (!created || created->is_error) return *error = "couldn't start the change set: " + (created ? json::dump(created->value) : "no tool"), std::nullopt;
  std::string id = text_of(created->value.as_object(), "id");
  bj::object add{{"id", id}};
  if (!versions.empty()) add["versions"] = versions;
  if (!files.empty()) add["content"] = bj::array{bj::object{{"tenant", tenant}, {"files", files}}};
  auto added = mcp_->call("changeset_add", add);
  if (!added || added->is_error) {
    mcp_->call("changeset_discard", bj::object{{"id", id}});
    return *error = "couldn't fill the change set: " + (added ? json::dump(added->value) : "no tool"), std::nullopt;
  }
  auto preview = mcp_->call("changeset_preview", bj::object{{"id", id}});
  audit("promotion.start", current_operator(), tenant, "ok",
        bj::object{{"changeset", id}, {"boxes", int64_t(versions.size())}, {"files", int64_t(files.size())}, {"why", why}});
  return bj::object{{"changeset", id},
                    {"draft_version", added->value.as_object().contains("draft_version") ? added->value.at("draft_version") : bj::value(nullptr)},
                    {"boxes", std::move(versions)},
                    {"files", int64_t(files.size())},
                    {"preview", preview && !preview->is_error ? preview->value : bj::value(nullptr)},
                    {"next", "changeset_apply {id: \"" + id + "\", version} makes it live (from the panel's Promote view, the owner's alone); changeset_rollback undoes it"}};
}

std::optional<bj::object> Host::promotion_pin(const std::string& tenant, const bj::object& args, std::string* error) {
  auto plan = promotion_plan(tenant, error);
  if (!plan) return std::nullopt;
  bool boxes_given = false, paths_given = false;
  auto only_boxes = names_of(args, "boxes", &boxes_given);
  auto only_paths = names_of(args, "paths", &paths_given);
  bj::object boxes, files;
  for (const auto& b : plan->at("boxes").as_array()) {
    std::string box = text_of(b.as_object(), "box");
    if (!boxes_given || only_boxes.count(box)) boxes[box] = b.at("to");
  }
  std::string beta = text_of(*plan, "beta_tenant");
  for (const auto& c : plan->at("content").as_array()) {
    std::string path = text_of(c.as_object(), "path");
    if (paths_given && !only_paths.count(path)) continue;
    if (auto f = content_->get(beta, path)) files[path] = base::sha256_hex(f->data);
  }
  if (boxes.empty() && files.empty()) return *error = "nothing to promote: beta and " + tenant + " are the same, or nothing was chosen", std::nullopt;
  return bj::object{{"boxes", std::move(boxes)}, {"files", std::move(files)}};
}

std::optional<bj::object> Host::promotion_request_run(const std::string& tenant, const bj::object& args, std::string* error) {
  // Only with an owner's approval (panel_decide sets who approved on this thread).
  std::string owner = approved_by_now();
  if (owner.empty()) return *error = "a promotion request runs only when a host:owner approves it in the panel's Promote view", std::nullopt;
  const auto* pinned = args.if_contains("pinned");
  if (!pinned || !pinned->is_object()) return *error = "the request holds no pinned plan; ask again", std::nullopt;
  // What the owner approved is what beta was when asked: anything changed since refuses it.
  bj::object now_args;
  bj::array box_names, paths;
  for (const auto& [box, v] : pinned->at("boxes").as_object()) box_names.push_back(bj::value(box));
  for (const auto& [path, v] : pinned->at("files").as_object()) paths.push_back(bj::value(path));
  now_args["boxes"] = box_names;
  now_args["paths"] = paths;
  auto now = promotion_pin(tenant, now_args, error);
  if (!now) return std::nullopt;
  if (json::dump(*now) != json::dump(*pinned)) {
    return *error = "beta changed since the request: its plan isn't the one approved (nothing was promoted); ask again", std::nullopt;
  }
  std::string why = "requested by " + current_operator() + ", approved by " + owner + ": " + text_of(args, "why");
  bj::object start{{"boxes", box_names}, {"paths", paths}, {"why", why}};
  std::string request_id = current_request_id();
  OperatorContext as(*this, owner, request_id);
  auto started = promotion_start(tenant, start, error);
  if (!started) return std::nullopt;
  bj::object apply{{"id", started->at("changeset")}};
  if (started->at("draft_version").is_int64()) apply["version"] = started->at("draft_version");
  auto applied = mcp_->call("changeset_apply", apply);
  if (!applied || applied->is_error) {
    return *error = "change set " + text_of(*started, "changeset") + " didn't apply (nothing stayed changed): " +
                    (applied ? json::dump(applied->value) : std::string("no tool")),
           std::nullopt;
  }
  return bj::object{{"changeset", started->at("changeset")}, {"applied", true}, {"approved_by", owner}, {"result", applied->value}};
}

void Host::register_promotion_tools() {
  using mcp::object_schema;
  using mcp::string_property;
  auto list = [](const char* d) { return bj::object{{"type", "array"}, {"items", bj::object{{"type", "string"}}}, {"description", d}}; };

  mcp_->add_tool(
      {"promotion_preview",
       "Read tool. What beta changed that production lacks, for one site (its production tenant): boxes whose beta "
       "version differs from the live one (each with version_diff's review and widens, or a new box), content files "
       "beta has that production lacks or has otherwise (new or changed, with before and after when text), files "
       "only production has (left alone), warnings, and what is never carried (data, accounts, keys, flags, "
       "attachments). design/machine/053.Beta.md § 11.",
       object_schema({{"tenant", string_property("The production tenant, like acme.")}}, {"tenant"}),
       [this](const bj::object& args) {
         std::string err;
         auto plan = promotion_plan(text_of(args, "tenant"), &err);
         if (!plan) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*plan));
       }});

  mcp_->add_tool(
      {"promotion_request",
       "Asks the owner to promote beta to production for one site: what beta has now (all of promotion_preview's, "
       "or only the boxes and paths named) is pinned, and the request always waits for a host:owner, whatever your "
       "approvals switch says. It answers pending_approval; the owner approves it in the panel's Promote view by "
       "typing the tenant, and it then runs as one change set (042), applied at once, which changeset_rollback "
       "undoes. Refused when beta changed since you asked: ask again. Beta's operator may ask for its own sites. "
       "approval_status {id} follows it. design/machine/053.Beta.md § 11.",
       object_schema({{"tenant", string_property("The production tenant, like acme.")},
                      {"boxes", list("Only these boxes.")},
                      {"paths", list("Only these content paths.")},
                      {"why", string_property("What the promotion carries and why it's ready, for the owner.")},
                      {"pinned", mcp::any_property("Set by the host when you ask: what beta has now. Anything given is replaced.")}},
                     {"tenant", "why"}),
       [this](const bj::object& args) {
         std::string err;
         auto out = promotion_request_run(text_of(args, "tenant"), args, &err);
         if (!out) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*out));
       }});

  mcp_->add_tool(
      {"promotion_start",
       "Makes a draft change set (042) promoting beta to production for one site: the boxes' beta versions and beta's "
       "content files (all of promotion_preview's, or only boxes and paths named), previewed. Nothing is live until "
       "changeset_apply (in the panel, the Promote view is the owner's alone); changeset_rollback undoes it in one call. Answers the change set's id, "
       "its draft_version, and the preview.",
       object_schema({{"tenant", string_property("The production tenant.")},
                      {"boxes", list("Only these boxes.")},
                      {"paths", list("Only these content paths.")},
                      {"why", string_property("What the promotion carries, for the change set and the audit log.")}},
                     {"tenant", "why"}),
       [this](const bj::object& args) {
         std::string err;
         auto out = promotion_start(text_of(args, "tenant"), args, &err);
         if (!out) return ToolResult::fail(err);
         return ToolResult::ok(std::move(*out));
       }});
}

}  // namespace goober::host
