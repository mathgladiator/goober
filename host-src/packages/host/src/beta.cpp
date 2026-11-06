// Beta (design/machine/053.Beta.md): a beta site on the production host, linked to its
// production site by `beta-of`. A box's beta version answers on beta domains only (the registry's
// beta slot); version_upload and version_promote take channel "beta". This file has the tools
// that read and clear the beta slots.

#include <filesystem>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/names.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/mail_inbound/address.h"
#include "goober/mail_inbound/mime.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace fs = std::filesystem;
namespace inbound = mail::inbound;

namespace {

constexpr size_t kMailboxMax = 2000;              // messages per beta tenant, the newest kept
constexpr int64_t kMailboxMs = 30LL * 86400000;  // and none older than 30 days
constexpr size_t kMessageTextMax = 256 * 1024;   // each of a message's text and html, cut past this

std::string random_hex(size_t bytes) {
  std::vector<uint8_t> raw(bytes);
  base::os_random_fill(raw);
  return base::hex_encode(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
}

// A message id as the mailbox names its files: 13 digits, "-", 8 hex digits.
bool valid_id(std::string_view id) {
  if (id.size() != 22 || id[13] != '-') return false;
  for (size_t i = 0; i < id.size(); ++i) {
    char c = id[i];
    if (i == 13) continue;
    if (i < 13 ? !(c >= '0' && c <= '9') : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

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
                    {"beta_ops", cfg->beta_panel_domain().empty() ? bj::value(nullptr) : bj::value(cfg->beta_panel_domain())},
                    {"boxes", std::move(boxes)}};
}

std::string Host::beta_mail_keep(const std::string& tenant, bj::object message) {
  fs::path dir = config_.get()->storage / "mail" / "beta" / tenant;
  std::error_code ec;
  fs::create_directories(dir, ec);
  int64_t now = base::now_unix_ms();
  std::string at = std::to_string(now);
  std::string id = std::string(13 - std::min<size_t>(13, at.size()), '0') + at + "-" + random_hex(4);
  message["id"] = id;
  message["at_ms"] = now;
  message["tenant"] = tenant;
  if (!message.contains("message_id")) message["message_id"] = "<beta-" + id + "@" + tenant + ".beta>";
  // A message is a test's: its text and html are cut at kMessageTextMax, said so.
  for (const char* k : {"text", "html"}) {
    std::string v = text_of(message, k);
    if (v.size() > kMessageTextMax) {
      message[k] = v.substr(0, kMessageTextMax);
      message["truncated"] = true;
    }
  }
  // Its row for beta_mail_list, beside it, so a list never reads whole messages.
  std::string body = text_of(message, "text");
  bj::object head{{"id", id}, {"at_ms", now}, {"direction", message.at("direction")}, {"from", message.at("from")},
                  {"to", message.at("to")}, {"subject", message.at("subject")},
                  {"preview", body.size() > 160 ? body.substr(0, 160) + "…" : body}};
  if (message.contains("outcome")) head["outcome"] = message.at("outcome");
  base::write_file_durable(dir / (id + ".json"), json::dump(message));
  base::write_file_durable(dir / (id + ".head"), json::dump(head));
  // The newest kMailboxMax within limits.beta-mail-bytes, none past 30 days: names sort by time.
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::string n = e.path().filename().string();
    if (n.ends_with(".json")) names.push_back(n.substr(0, n.size() - 5));
  }
  std::sort(names.rbegin(), names.rend());
  int64_t budget = config_.get()->disk.beta_mail_bytes, bytes = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    std::error_code se;
    auto size = fs::file_size(dir / (names[i] + ".json"), se);
    bytes += se ? 0 : int64_t(size);
    bool old = std::strtoll(names[i].substr(0, 13).c_str(), nullptr, 10) < now - kMailboxMs;
    // The newest message stays whatever its size.
    if (i > 0 && (old || i >= kMailboxMax || bytes > budget)) {
      fs::remove(dir / (names[i] + ".json"), ec);
      fs::remove(dir / (names[i] + ".head"), ec);
    }
  }
  return id;
}

void Host::beta_capture(const identity::Mail& m, const std::string& tenant) {
  auto cfg = config_.get();
  std::string from = !m.from_address.empty() ? m.from_address
                     : cfg->identity.mailer == "ses" ? cfg->identity.ses.from
                                                     : std::string("goober@") + (cfg->platform_domain.empty() ? "localhost" : cfg->platform_domain);
  bj::object msg{{"direction", "out"},
                 {"from", from},
                 {"from_name", m.from_name},
                 {"to", m.to},
                 {"subject", m.subject},
                 {"text", m.text},
                 {"html", m.html},
                 {"reply_to", m.reply_to},
                 {"in_reply_to", m.in_reply_to},
                 {"references", m.references},
                 {"list_id", m.list_id},
                 {"sent_by", m.tenant.empty() ? "host" : "box"}};
  beta_mail_keep(tenant, std::move(msg));
}

bj::array Host::beta_mail_list(const std::string& tenant, int64_t limit) {
  bj::array out;
  if (!base::is_valid_name(tenant)) return out;
  fs::path dir = config_.get()->storage / "mail" / "beta" / tenant;
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::string n = e.path().filename().string();
    if (n.ends_with(".json")) names.push_back(n);
  }
  std::sort(names.rbegin(), names.rend());
  for (const auto& n : names) {
    if (int64_t(out.size()) >= limit) break;
    std::string err;
    // Its row, else (a message kept before rows were) the whole message.
    std::string stem = n.substr(0, n.size() - 5);
    if (auto head = base::read_file(dir / (stem + ".head"))) {
      if (auto h = json::parse(*head, &err); h && h->is_object()) {
        out.push_back(std::move(*h));
        continue;
      }
    }
    auto text = base::read_file(dir / n);
    auto v = text ? json::parse(*text, &err) : std::nullopt;
    if (!v || !v->is_object()) continue;
    auto& o = v->as_object();
    std::string body = text_of(o, "text");
    bj::object row{{"id", o.at("id")},     {"at_ms", o.at("at_ms")},       {"direction", o.at("direction")},
                   {"from", o.at("from")}, {"to", o.at("to")},             {"subject", o.at("subject")},
                   {"preview", body.size() > 160 ? body.substr(0, 160) + "…" : body}};
    if (o.contains("outcome")) row["outcome"] = o.at("outcome");
    out.push_back(std::move(row));
  }
  return out;
}

std::optional<bj::object> Host::beta_mail_get(const std::string& tenant, const std::string& id) {
  if (!base::is_valid_name(tenant) || !valid_id(id)) return std::nullopt;
  auto text = base::read_file(config_.get()->storage / "mail" / "beta" / tenant / (id + ".json"));
  std::string err;
  auto v = text ? json::parse(*text, &err) : std::nullopt;
  if (!v || !v->is_object()) return std::nullopt;
  return v->as_object();
}

bj::object Host::beta_mail_inject(const std::string& to_in, const std::string& from_in, const std::string& subject,
                                  const std::string& text, const std::string& in_reply_to, std::string* error) {
  auto cfg = config_.get();
  std::string to = base::to_lower(to_in), from = base::to_lower(from_in);
  auto parts = inbound::split_address(to);
  if (!parts) return *error = "to isn't an address", bj::object{};
  if (!inbound::split_address(from)) return *error = "from isn't an address", bj::object{};
  if (cfg->beta_of(parts->second).empty()) return *error = parts->second + " isn't a beta domain: only beta takes injected mail", bj::object{};
  // Only into a beta the operator is focused on (030, 053 § 6), judged by the address's domain.
  std::string domain_tenant = cfg->tenant_for_host(parts->second).value_or("");
  if (auto scope = operator_scope(current_operator()); !scope.all && !scope.tenants.count(domain_tenant)) {
    return *error = "operator " + current_operator() + " isn't focused on " + domain_tenant + ", whose address " + to + " is",
           bj::object{};
  }
  std::string why;
  auto match = match_mail(to, &why);
  if (!match) return *error = "no box takes mail at " + to + " on beta (" + why + ")", bj::object{};
  std::string tenant = match->tenant;
  // Threading: a reply names a message of this mailbox by its id.
  std::string reply_mid, references;
  if (!in_reply_to.empty()) {
    auto orig = beta_mail_get(tenant, in_reply_to);
    if (!orig) return *error = "in_reply_to " + in_reply_to + " isn't a message of " + tenant + "'s beta mailbox", bj::object{};
    reply_mid = text_of(*orig, "message_id");
    references = (text_of(*orig, "references").empty() ? "" : text_of(*orig, "references") + " ") + reply_mid;
  }
  std::string id_part = random_hex(8);
  std::string message_id = "<inject-" + id_part + "@" + parts->second + ">";
  inbound::Headers headers = {{"From", from},
                              {"To", to},
                              {"Subject", subject},
                              {"Date", base::format_utc_ms(base::now_unix_ms())},
                              {"Message-ID", message_id},
                              {"MIME-Version", "1.0"},
                              {"Content-Type", "text/plain; charset=utf-8"}};
  if (!reply_mid.empty()) {
    headers.emplace_back("In-Reply-To", reply_mid);
    headers.emplace_back("References", references);
  }
  std::string body;
  for (char c : text) {
    if (c == '\n') body += "\r\n";
    else if (c != '\r') body += c;
  }
  bj::object log{{"mail_in", "beta"}, {"from", from}, {"to", to}, {"tenant", tenant}};
  std::string request_id = "beta-" + random_hex(8);
  auto [reply, outcome] = deliver_mail_in(*match, from, headers, body, int64_t(body.size()), request_id, log, true);
  bj::object kept{{"direction", "in"},     {"from", from},          {"to", to},           {"subject", subject},
                  {"text", text},          {"html", ""},            {"message_id", message_id},
                  {"in_reply_to", reply_mid}, {"references", references}, {"outcome", outcome},
                  {"answer", reply},       {"box", match->ref.box->manifest.box}, {"route", match->ref.route->name},
                  {"injected_by", current_operator()}};
  std::string id = beta_mail_keep(tenant, kept);
  audit("beta.mail_inject", current_operator(), tenant + "/" + to, "ok", bj::object{{"outcome", outcome}, {"id", id}});
  return bj::object{{"id", id}, {"tenant", tenant}, {"outcome", outcome}, {"answer", reply},
                    {"box", match->ref.box->manifest.box}, {"route", match->ref.route->name},
                    {"next", "the box's replies, if any, are in beta_mail_list as direction out"}};
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
      {"beta_mail_list",
       "Read tool. A beta tenant's mailbox, newest first (053 § 4): direction out (what the site would have sent: "
       "sign-in codes, invites, a box's mail, captured instead) and in (what an operator injected, with the outcome). "
       "Each {id, at_ms, direction, from, to, subject, preview}. Nothing from beta ever leaves the machine.",
       object_schema({{"tenant", string_property("The beta tenant.")},
                      {"limit", mcp::integer_property("How many, 1 to 500; 50 when left out.")}},
                     {"tenant"}),
       [this](const bj::object& args) {
         std::string tenant = text_of(args, "tenant");
         if (!config_.get()->is_beta_tenant(tenant)) return ToolResult::fail(tenant + " isn't a beta tenant (beta_status names them)");
         auto limit = mcp::integer_arg(args, "limit");
         return ToolResult::ok(beta_mail_list(tenant, std::clamp<int64_t>(limit.ok ? limit.value : 50, 1, 500)));
       }});

  mcp_->add_tool(
      {"beta_mail_get",
       "Read tool. One message of a beta tenant's mailbox, whole: from, to, subject, text, html (as its source), "
       "reply_to (where a reply goes: often a reply-token address), message_id, in_reply_to, references, and for "
       "an injected one its outcome and the SMTP answer the box gave.",
       object_schema({{"tenant", string_property("The beta tenant.")}, {"id", string_property("The message's id.")}},
                     {"tenant", "id"}),
       [this](const bj::object& args) {
         std::string tenant = text_of(args, "tenant");
         if (!config_.get()->is_beta_tenant(tenant)) return ToolResult::fail(tenant + " isn't a beta tenant");
         auto m = beta_mail_get(tenant, text_of(args, "id"));
         if (!m) return ToolResult::fail("no message " + text_of(args, "id") + " in " + tenant + "'s beta mailbox");
         return ToolResult::ok(std::move(*m));
       }});

  mcp_->add_tool(
      {"beta_mail_send",
       "Injects a message to a beta address, as if it arrived by mail (053 § 4): the box's mail route gets it as the "
       "MTA hook would deliver it, with its From taken as verified, and its answer (delivered, rejected, deferred) "
       "comes back as the outcome. Reply to a captured message with in_reply_to (its id) and to = its reply_to (or "
       "from). Text only. Audited.",
       object_schema({{"to", string_property("A beta address, like support@beta.example.com, or a reply token's.")},
                      {"from", string_property("Who it is from, like ann@example.org.")},
                      {"subject", string_property("The subject.")},
                      {"text", string_property("The body, as plain text.")},
                      {"in_reply_to", string_property("The id of the mailbox message this answers.")}},
                     {"to", "from", "subject", "text"}),
       [this](const bj::object& args) {
         std::string err;
         auto out = beta_mail_inject(text_of(args, "to"), text_of(args, "from"), text_of(args, "subject"), text_of(args, "text"),
                                     text_of(args, "in_reply_to"), &err);
         if (!err.empty()) return ToolResult::fail(err);
         return ToolResult::ok(std::move(out));
       }});

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
