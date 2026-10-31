// What an operator needs to see about a tenant's mail without leaving the tools
// (design/machine/024.MailIn.md § 7, 018.Mail.md § 6): the domain's mail DNS and SES identity
// (mail_domain_check), which route takes an address (mail_route_check), and whether the SMTP
// listener mail in runs on answers (host_health's smtp_listener).

#include <algorithm>
#include <set>

#include "dns.h"
#include "keys.h"
#include "goober/base/names.h"
#include "goober/host/audit.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/mail_inbound/address.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

bj::array strings(const std::vector<std::string>& v) {
  bj::array a;
  for (const auto& s : v) a.emplace_back(s);
  return a;
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

std::string Host::postal_address_how(const std::string& tenant) {
  return "a person sets tenants." + tenant +
         ".postal-address: the deploy's settings step asks for it, or edit host.json; no tool sets it";
}

bj::object Host::smtp_listener() {
  auto cfg = config_.get();
  if (!cfg->mail.inbound.enabled()) return bj::object{{"configured", false}};
  std::string err;
  uint16_t port = smtp_port_.load();
  auto banner = dns::smtp_banner("127.0.0.1", port, 1500, &err);
  bj::object out{{"configured", true}, {"address", "127.0.0.1:" + std::to_string(port)}, {"up", banner.has_value()}};
  if (banner) out["banner"] = *banner;
  else out["error"] = err;
  return out;
}

void Host::register_mail_check_tools() {
  using mcp::object_schema;
  using mcp::string_property;
  auto domain_prop = string_property("One of the host's tenant domains, like example.com.");

  mcp_->add_tool(
      {"mail_domain_check",
       "A tenant domain's mail DNS and sending identity, checked live, with the records to add for anything "
       "missing: MX (and whether it points at this host, whose port 25 is then tried from here), SPF, DMARC, and, "
       "when mail goes out through SES, the domain's SES identity, its verification, and its DKIM tokens. Only the "
       "host's own domains, and port 25 only on this host's own addresses.",
       object_schema({{"domain", domain_prop}}, {"domain"}),
       [this](const bj::object& args) {
         auto cfg = config_.get();
         std::string domain = lower(arg(args, "domain").value_or(""));
         auto entry = cfg->domains.find(domain);
         if (entry == cfg->domains.end()) return ToolResult::fail(domain + " isn't a tenant domain in the host config");
         bj::array findings;
         auto find = [&](const char* level, std::string text, std::string fix = "") {
           bj::object f{{"level", level}, {"text", std::move(text)}};
           if (!fix.empty()) f["fix"] = std::move(fix);
           findings.push_back(std::move(f));
         };
         bool mail_in = cfg->mail.inbound.enabled();
         std::string mx_target = cfg->platform_domain;
         // This server, as the platform domain resolves.
         auto ours = resolve_addresses(cfg->platform_domain);
         bj::object out{{"domain", domain}, {"tenant", entry->second.tenant}, {"mailer", cfg->identity.mailer},
                        {"mail_in", mail_in}};

         // MX: where mail for the domain goes.
         std::string err;
         bj::array mx_list;
         bool mx_here = false;
         std::string probe_address;
         if (auto mx = dns::lookup_mx(domain, &err)) {
           for (const auto& m : *mx) {
             auto addrs = resolve_addresses(m.host);
             bool here = !addrs.empty() && std::all_of(addrs.begin(), addrs.end(), [&](const std::string& a) {
               return std::find(ours.begin(), ours.end(), a) != ours.end();
             });
             if (here && probe_address.empty() && !addrs.empty()) probe_address = addrs.front();
             mx_here |= here;
             mx_list.push_back(bj::object{{"preference", m.preference}, {"host", m.host}, {"addresses", strings(addrs)},
                                          {"this_host", here}});
           }
           out["mx"] = std::move(mx_list);
           if (mail_in && mx->empty()) {
             find("warn", "no MX record: mail for " + domain + " goes nowhere",
                  domain + ".  MX  10 " + mx_target + ".");
           } else if (mail_in && !mx_here) {
             find("warn", "the MX records don't point at this host, so its mail routes never see mail",
                  domain + ".  MX  10 " + mx_target + ".");
           }
         } else {
           out["mx_error"] = err;
         }
         // Port 25 on this host's own address, as the MX names it (never anyone else's).
         if (!probe_address.empty()) {
           std::string perr;
           auto banner = dns::smtp_banner(probe_address, smtp_port_.load(), 3000, &perr);
           bj::object p{{"address", probe_address}, {"answers", banner.has_value()}};
           if (banner) p["banner"] = *banner;
           else p["error"] = perr;
           p["note"] = "tried from this host; a provider's firewall can still block it from outside";
           out["port_25"] = std::move(p);
           if (!banner) find("warn", "port 25 on " + probe_address + " didn't answer: " + perr);
         }

         // SPF and DMARC.
         bool ses = cfg->identity.mailer == "ses";
         if (auto txt = dns::lookup_txt(domain, &err)) {
           std::vector<std::string> spf;
           for (const auto& t : *txt) {
             if (lower(t).starts_with("v=spf1")) spf.push_back(t);
           }
           out["spf"] = spf.empty() ? bj::value(nullptr) : bj::value(spf.front());
           if (spf.empty()) {
             find("suggest", "no SPF record", domain + ".  TXT  \"v=spf1 " + std::string(ses ? "include:amazonses.com " : "") + "~all\"");
           } else if (spf.size() > 1) {
             find("warn", "more than one SPF record: receivers treat that as none (RFC 7208 § 3.2)");
           } else if (ses && lower(spf.front()).find("include:amazonses.com") == std::string::npos) {
             find("suggest", "SPF doesn't include amazonses.com, which sends this host's mail",
                  "add include:amazonses.com to " + domain + "'s v=spf1 record");
           }
         } else {
           out["spf_error"] = err;
         }
         if (auto txt = dns::lookup_txt("_dmarc." + domain, &err)) {
           std::string dmarc;
           for (const auto& t : *txt) {
             if (lower(t).starts_with("v=dmarc1")) dmarc = t;
           }
           out["dmarc"] = dmarc.empty() ? bj::value(nullptr) : bj::value(dmarc);
           if (dmarc.empty()) {
             find("suggest", "no DMARC record", "_dmarc." + domain + ".  TXT  \"v=DMARC1; p=none; rua=mailto:dmarc@" + domain + "\"");
           }
         } else {
           out["dmarc_error"] = err;
         }

         // The SES identity: may this domain send, and is its DKIM in place?
         if (!ses) {
           out["ses"] = bj::object{{"skipped", "mail goes out through " + cfg->identity.mailer + ", not SES"}};
         } else if (auto creds = keys_->aws_credentials(); !creds) {
           out["ses"] = bj::object{{"skipped", "no AWS keys (keys/system/aws_access_key_id and aws_secret_access_key)"}};
         } else {
           const auto& s = cfg->identity.ses;
           auto id = aws::ses_identity(domain, *creds, s.region.empty() ? cfg->aws.region : s.region, cfg->storage / "tmp",
                                       s.endpoint);
           if (!id.ok) {
             out["ses"] = bj::object{{"error", id.error}};
           } else if (!id.exists) {
             out["ses"] = bj::object{{"exists", false}};
             find("warn", domain + " isn't an identity in SES, so mail from it can't be sent through SES",
                  "create the domain identity in the SES console (Easy DKIM), then add the three CNAME records it shows");
           } else {
             bj::array records;
             for (const auto& t : id.dkim_tokens) {
               records.emplace_back(t + "._domainkey." + domain + ".  CNAME  " + t + ".dkim.amazonses.com.");
             }
             out["ses"] = bj::object{{"exists", true},
                                     {"verified_for_sending", id.verified_for_sending},
                                     {"verification_status", id.verification_status},
                                     {"dkim_status", id.dkim_status},
                                     {"dkim_signing", id.dkim_signing},
                                     {"dkim_tokens", strings(id.dkim_tokens)},
                                     {"dkim_records", records}};
             if (id.dkim_status != "SUCCESS") {
               std::string all;
               for (const auto& r : records) all += (all.empty() ? "" : "; ") + std::string(r.as_string());
               find("warn", "SES's DKIM for " + domain + " is " + (id.dkim_status.empty() ? "not set up" : id.dkim_status),
                    all.empty() ? "turn on Easy DKIM for the identity in SES" : all);
             }
             if (!id.verified_for_sending) find("warn", "SES won't send from " + domain + " yet (not verified for sending)");
           }
         }
         if (cfg->tenant(entry->second.tenant).postal_address.empty()) {
           find("warn", "no postal address for " + entry->second.tenant + ": invites, lists, and boxes' mail to people are refused",
                postal_address_how(entry->second.tenant));
         }
         out["findings"] = std::move(findings);
         return ToolResult::ok(std::move(out));
       }});

  mcp_->add_tool(
      {"mail_route_check",
       "Which box, version, and mail route would take mail for an address, exactly as the host decides when "
       "Stalwart asks (specific addresses before a catch-all *@), or why it would be refused at RCPT. Give a "
       "domain and an address or just its local part. A reply address's token is checked as for real mail.",
       object_schema({{"domain", domain_prop},
                      {"address", string_property("An address, like support@example.com, or a local part, like support.")}},
                     {"domain", "address"}),
       [this](const bj::object& args) {
         auto cfg = config_.get();
         std::string domain = lower(arg(args, "domain").value_or(""));
         std::string address = lower(arg(args, "address").value_or(""));
         if (!cfg->domains.count(domain)) return ToolResult::fail(domain + " isn't a tenant domain in the host config");
         if (address.find('@') == std::string::npos) address += "@" + domain;
         if (address.substr(address.rfind('@') + 1) != domain) {
           return ToolResult::fail("the address is on " + address.substr(address.rfind('@') + 1) + ", not " + domain);
         }
         bj::object out{{"address", address}, {"domain", domain}, {"tenant", cfg->domains.at(domain).tenant},
                        {"mail_in", cfg->mail.inbound.enabled()}};
         std::string why;
         auto m = match_mail(address, &why);
         if (!m) {
           out["taken"] = false;
           out["refused_at"] = "RCPT";
           out["why"] = why;
         } else {
           out["taken"] = true;
           out["box"] = m->ref.box->manifest.box;
           out["version"] = m->ref.box->version;
           out["route"] = m->ref.route->name;
           out["pattern"] = m->ref.route->mail_address;
           std::string err;
           auto p = mail::inbound::parse_address_pattern(m->ref.route->mail_address, &err);
           out["catch_all"] = p && p->catch_all;
         }
         if (!cfg->mail.inbound.enabled()) {
           out["note"] = "mail in isn't set up on this host (mail.inbound.token-file): Stalwart never asks";
         }
         return ToolResult::ok(std::move(out));
       }});
}

}  // namespace goober::host
