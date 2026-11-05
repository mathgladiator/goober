// Beta (design/machine/053.Beta.md): a beta site on the production host, linked to its
// production site by `beta-of`. A box's beta version answers on beta domains only (the registry's
// beta slot); version_upload and version_promote take channel "beta". This file has the tools
// that read and clear the beta slots.

#include "goober/base/time.h"
#include "goober/host/host.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

std::string text_of(const bj::object& o, const char* k) {
  const auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

}  // namespace

bj::object Host::beta_status() {
  auto cfg = config_.get();
  auto active = boxes_->active();
  bj::array sites;
  for (const auto& [domain, e] : cfg->domains) {
    if (e.beta_of.empty()) continue;
    auto prod = cfg->domains.find(e.beta_of);
    sites.push_back(bj::object{{"domain", domain},
                               {"tenant", e.tenant},
                               {"beta_of", e.beta_of},
                               {"production_tenant", prod == cfg->domains.end() ? "" : prod->second.tenant}});
  }
  bj::array boxes;
  for (const auto& [name, box] : active->versions(true)) {
    auto live = active->boxes.find(name);
    auto beta = active->beta.find(name);
    bj::object row{{"box", name},
                   {"live_version", live == active->boxes.end() ? bj::value(nullptr) : bj::value(live->second->version)},
                   {"beta_version", beta == active->beta.end() ? bj::value(nullptr) : bj::value(beta->second->version)}};
    row["state"] = beta == active->beta.end()         ? "follows live"
                   : live == active->boxes.end()        ? "beta only"
                   : beta->second->version == live->second->version ? "same as live"
                                                                     : "differs from live";
    bj::array domains;
    for (const auto& d : box->manifest.domains) domains.push_back(bj::value(d));
    row["domains"] = std::move(domains);
    boxes.push_back(std::move(row));
  }
  return bj::object{{"sites", std::move(sites)},
                    {"beta_ops", cfg->operations_beta_domain.empty() ? bj::value(nullptr) : bj::value(cfg->operations_beta_domain)},
                    {"boxes", std::move(boxes)}};
}

void Host::register_beta_tools() {
  using mcp::object_schema;
  using mcp::string_property;

  mcp_->add_tool(
      {"beta_status",
       "Read tool. The beta sites (each beta domain, its tenant, and the production domain it is the beta of) and "
       "beta-ops' domain; and every box with its live and beta versions: state is \"follows live\" (no beta "
       "version: beta answers with the live one), \"differs from live\", \"same as live\", or \"beta only\" (a box "
       "not live in production yet). Stage a version for beta with version_upload {channel: \"beta\"} or "
       "version_promote {box, version, channel: \"beta\"} (design/machine/053.Beta.md).",
       object_schema({}, {}), [this](const bj::object&) { return ToolResult::ok(beta_status()); }});

  mcp_->add_tool(
      {"beta_clear",
       "Clears a box's beta version: beta answers with the live version again (a box only in beta stops answering "
       "there). The version stays stored. Audited.",
       object_schema({{"box", string_property("The box.")}, {"why", string_property("Why, for the audit log.")}}, {"box"}),
       [this](const bj::object& args) {
         std::string box = text_of(args, "box"), err;
         auto was = boxes_->beta_version(box);
         if (!boxes_->beta_clear(box, &err)) return ToolResult::fail(err);
         audit("beta.clear", current_operator(), "box:" + box, "ok", bj::object{{"version", was.value_or("")}, {"why", text_of(args, "why")}});
         return ToolResult::ok(bj::object{{"box", box}, {"cleared", was.value_or("")}});
       }});
}

}  // namespace goober::host
