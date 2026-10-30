// What a tenant's mail did (mail_activity), and mail_unsubscribed, kept as a shim over the
// blocks of design/machine/029.Invites.md § 6 (consent.cpp holds the rules and the pages).

#include <algorithm>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/sqlite/sqlite.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

void Host::register_mail_out_tools() {
  using mcp::boolean_property;
  using mcp::object_schema;
  using mcp::string_property;
  mcp_->add_tool(
      {"mail_activity",
       "A tenant's recent mail, newest last: what arrived for its addresses (from, to, which box and route took "
       "it, and the answer: delivered, refused, deferred, dropped as automatic) and what its boxes sent (to, from, "
       "subject, sent or failed and why). Use it to answer \"did my mail arrive?\" and \"did my reply go?\". "
       "Mail a box queued but the host hasn't sent yet is in outbox_list. Logs keep addresses only as hashes (032): "
       "each entry shows the address's domain and hash; give email to see only that address's mail, by name.",
       object_schema({{"tenant", string_property("A tenant name from the host config.")},
                      {"email", string_property("Only this address's mail.")},
                      {"limit", mcp::integer_property("How many entries, 1 to 200. Default 30.")}},
                     {"tenant"}),
       [this](const bj::object& args) {
         auto cfg = config_.get();
         auto* t = args.if_contains("tenant");
         std::string tenant = t && t->is_string() ? std::string(t->as_string()) : "";
         if (!base::is_valid_name(tenant)) return ToolResult::fail("a tenant name is required");
         int64_t limit = 30;
         if (auto* l = args.if_contains("limit"); l && l->is_int64()) limit = std::clamp<int64_t>(l->as_int64(), 1, 200);
         std::set<std::string> domains;
         for (const auto& [name, entry] : cfg->domains) {
           if (entry.tenant == tenant) domains.insert(name);
         }
         auto str = [](const bj::object& o, const char* k) {
           auto* v = o.if_contains(k);
           return v && v->is_string() ? std::string(v->as_string()) : std::string();
         };
         // An address as the log keeps it (a hash and a domain), or the asked-for address when it matches.
         std::string want;
         if (auto* e = args.if_contains("email"); e && e->is_string()) want = base::to_lower(std::string(e->as_string()));
         std::string want_h = want.empty() ? "" : address_hash_for_logs(want);
         auto who = [&](const bj::object& o, const std::string& k) -> std::string {
           std::string h = str(o, (k + "_h").c_str());
           if (h.empty()) return str(o, k.c_str());  // lines from before 032
           if (!want_h.empty() && h == want_h) return want;
           return "#" + h + "@" + str(o, (k + "_domain").c_str());
         };
         auto matches = [&](const bj::object& o) {
           if (want.empty()) return true;
           for (const char* k : {"to", "from"}) {
             if (str(o, (std::string(k) + "_h").c_str()) == want_h || base::to_lower(str(o, k)) == want) return true;
           }
           return false;
         };
         auto when = [&](const bj::object& o) { return str(o, "event_at").empty() ? str(o, "at") : str(o, "event_at"); };
         std::vector<std::pair<std::string, bj::object>> out;
         std::string err;
         // Mail in: the hook's lines in the access log. A refused recipient has no tenant yet,
         // so its address's domain says whose it was.
         for (const auto& line : base::tail_lines(cfg->storage / "logs" / "access.log", 20000)) {
           if (line.find("\"mail_in\"") == std::string::npos) continue;
           auto v = json::parse(line, &err);
           if (!v || !v->is_object()) continue;
           const auto& o = v->as_object();
           std::string to = who(o, "to"), outcome = str(o, "outcome");
           if (to.empty() || outcome == "pass" || !matches(o)) continue;
           std::string owner = str(o, "tenant");
           std::string to_domain = str(o, "to_domain").empty() ? to.substr(to.find('@') + 1) : str(o, "to_domain");
           if (owner.empty()) owner = domains.count(to_domain) ? tenant : "";
           if (owner != tenant) continue;
           bj::object e{{"at", when(o)}, {"direction", "in"}, {"stage", str(o, "mail_in")}, {"from", who(o, "from")},
                        {"to", to}, {"outcome", outcome}};
           for (const char* k : {"box", "route", "why"}) {
             if (auto s = str(o, k); !s.empty()) e[k] = s;
           }
           if (auto* s = o.if_contains("status"); s && s->is_int64() && str(o, "mail_in") == "data") e["box_status"] = *s;
           out.emplace_back(when(o), std::move(e));
         }
         // Mail out: what the mailer did with each of the tenant's boxes' mails.
         for (const auto& line : base::tail_lines(cfg->storage / "logs" / "mail.log", 5000)) {
           auto v = json::parse(line, &err);
           if (!v || !v->is_object() || str(v->as_object(), "tenant") != tenant || !matches(v->as_object())) continue;
           const auto& o = v->as_object();
           bj::object e{{"at", when(o)}, {"direction", "out"}, {"to", who(o, "to")}, {"subject", str(o, "subject")},
                        {"outcome", str(o, "status")}};
           if (auto f = str(o, "from_address"); !f.empty()) e["from"] = f;
           else if (auto g = who(o, "from"); !g.empty()) e["from"] = g;
           for (const char* k : {"in_reply_to", "error", "message_id"}) {
             if (auto s = str(o, k); !s.empty()) e[k] = s;
           }
           out.emplace_back(when(o), std::move(e));
         }
         std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
         bj::array list;
         size_t from = out.size() > size_t(limit) ? out.size() - size_t(limit) : 0;
         for (size_t i = from; i < out.size(); ++i) list.push_back(std::move(out[i].second));
         return ToolResult::ok(bj::object{{"tenant", tenant}, {"mail", std::move(list)}});
       }});

  mcp_->add_tool(
      {"mail_unsubscribed",
       "Whether an address is blocked for a tenant: it unsubscribed from everything, declined an invite, or was "
       "blocked by an operator. Addresses are kept only as hashes, so give an email to check one; without one, "
       "the count. set: true blocks an address (they asked by phone or reply). Lifting a block takes a person: "
       "them, signed in on the site, or an operator in the panel. No tool lifts one. Prefer mail_preferences.",
       object_schema({{"tenant", string_property("A tenant name from the host config.")},
                      {"email", string_property("An address to check or block.")},
                      {"set", boolean_property("true blocks the address. false is refused: lift blocks in the panel.")}},
                     {"tenant"}),
       [this](const bj::object& args) {
         auto str = [&](const char* k) {
           auto* v = args.if_contains(k);
           return v && v->is_string() ? std::string(v->as_string()) : std::string();
         };
         std::string tenant = str("tenant"), email = base::to_lower(str("email"));
         if (!base::is_valid_name(tenant)) return ToolResult::fail("a tenant name is required");
         if (email.empty()) {
           return ToolResult::ok(bj::object{{"tenant", tenant}, {"note", "give an email to check one address"}});
         }
         if (email.find('@') == std::string::npos) return ToolResult::fail("email is one address");
         if (auto* set = args.if_contains("set"); set && set->is_bool()) {
           if (!set->as_bool()) {
             return ToolResult::fail("a block is lifted by a person: the address's owner on their account page, or an "
                                     "operator in the panel's Operators view, saying why");
           }
           consent_->add_block(tenant, mail::consent::address_hash(email), "operator",
                               bj::object{{"by", current_operator()}}, base::now_unix_ms());
         }
         return ToolResult::ok(bj::object{{"tenant", tenant}, {"email", email}, {"unsubscribed", mail_suppressed(tenant, email)}});
       }});
}

}  // namespace goober::host
