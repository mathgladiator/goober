// Who a tenant may mail (design/machine/029.Invites.md §§ 3–6): every box mail has a category
// with its own rule, lists need a recorded opt-in, a block stops everything until a person lifts
// it, and the host's footer says why each mail came and how to stop it. The preferences page
// every footer links to only ever reduces mail; joining a list or lifting a block takes a
// signed-in person or a confirm mail.

#include <algorithm>
#include <iostream>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/router/path.h"
#include "goober/sqlite/sqlite.h"
#include "http_util.h"
#include "mail_hash.h"
#include "network.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
using base::html_escape;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;
using mail::consent::address_hash;

namespace {

constexpr int64_t kDay = 24LL * 3600 * 1000;

bool same(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
  return d == 0;
}

bool is_hex(std::string_view s, size_t n) {
  if (s.size() != n) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool is_ip_or_local(const std::string& d) {
  if (d == "localhost" || d.ends_with(".localhost")) return true;
  return !d.empty() && (std::isdigit(static_cast<unsigned char>(d[0])) || d[0] == '[');
}

std::string field(const bj::object& o, const char* k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

}  // namespace

bool Host::mail_suppressed(const std::string& tenant, const std::string& address) {
  return consent_->block_of(tenant, address).has_value();
}

std::string Host::site_of(const std::string& tenant) const {
  auto cfg = config_.get();
  std::string fallback;
  for (const auto& [domain, entry] : cfg->domains) {
    if (entry.tenant != tenant) continue;
    if (!is_ip_or_local(domain)) return domain;
    if (fallback.empty()) fallback = domain;
  }
  return fallback;
}

std::string Host::mail_link(const std::string& domain, const std::string& tenant, const std::string& address,
                            const std::string& scope) {
  auto cfg = config_.get();
  std::string hash = address_hash(address);
  // The same MAC as the links mailed before 029, so they keep working.
  std::string mac = base::hmac_sha256_hex(reply_key(), "goober-unsubscribe\n" + tenant + "\n" + hash).substr(0, 32);
  // On the site the mail is about, so the person sees its name; on this machine, the base URL.
  std::string base = cfg->local || domain.empty() || domain == "localhost" ? cfg->base_url() : "https://" + domain;
  return base + "/goober/mail/" + tenant + "/" + hash + "/" + mac + (scope.empty() ? "" : "/" + scope);
}

std::map<std::string, std::pair<manifest::ExternalService::MailList, std::string>> Host::tenant_lists(
    const std::string& tenant) {
  std::map<std::string, std::pair<manifest::ExternalService::MailList, std::string>> out;
  auto cfg = config_.get();
  auto active = boxes_->active();
  for (const auto& [name, box] : active->boxes) {
    bool serves = false;
    for (const auto& [t, d] : tenants_of(*cfg, box->manifest)) serves |= t == tenant;
    if (!serves) continue;
    for (const auto& svc : box->manifest.external) {
      for (const auto& [list, l] : svc.mail_lists) out.try_emplace(list, l, name);
    }
  }
  return out;
}

bool Host::wrote_in(const std::string& tenant, const std::string& address, int64_t within_ms) {
  // inbound.db is mail in's: one connection at a time, as everywhere else that opens it.
  std::lock_guard lock(mail_in_mu_);
  auto db = open_mail_db();
  sqlite::Stmt s(*db, "SELECT 1 FROM received WHERE tenant = ? AND sender = ? AND at_ms > ? LIMIT 1");
  s.bind(1, tenant).bind(2, received_hash(address)).bind(3, base::now_unix_ms() - within_ms);
  return s.step();
}

bool Host::thread_verified(const std::string& tenant, const std::string& address, const std::string& message_id) {
  if (message_id.empty()) return false;
  // inbound.db is mail in's: one connection at a time, as everywhere else that opens it.
  std::lock_guard lock(mail_in_mu_);
  auto db = open_mail_db();
  sqlite::Stmt s(*db, "SELECT 1 FROM received WHERE tenant = ? AND sender = ? AND message = ? AND at_ms > ?");
  s.bind(1, tenant).bind(2, received_hash(address)).bind(3, received_hash(message_id)).bind(4, base::now_unix_ms() - 30 * kDay);
  return s.step();
}

std::optional<Host::MailCheck> Host::check_mail_category(const std::string& tenant, const std::string& to,
                                                         std::string category, const std::string& list,
                                                         const std::string& in_reply_to, bool person_present,
                                                         bool at_delivery, std::string* why) {
  auto cfg = config_.get();
  std::string hash = address_hash(to);
  if (auto b = consent_->block_of(tenant, to)) {
    return *why = "blocked: " + to + " asked for no more mail from this site (" + b->how + ")", std::nullopt;
  }
  MailCheck c;
  c.list = list;
  bool account = identity_->proved(tenant, to);
  bool wrote = !account && wrote_in(tenant, to, 90 * kDay);
  c.relationship = account ? "account" : wrote ? "wrote_in" : "";
  bool threaded = thread_verified(tenant, to, in_reply_to);
  // A missing category is inferred strictly: never list or confirm (§ 3).
  if (category.empty()) category = threaded ? "reply" : !c.relationship.empty() ? "transactional" : "outreach";
  c.category = category;
  const std::string& postal = cfg->tenant(tenant).postal_address;
  auto need_postal = [&]() -> bool {
    if (!postal.empty()) return true;
    *why = category + " mail needs the tenant's postal address: set tenants." + tenant +
           ".postal-address in the host config (CAN-SPAM requires one in commercial mail)";
    return false;
  };
  if (category != "list" && !list.empty()) return *why = "list goes with \"category\": \"list\"", std::nullopt;
  if (category == "reply") {
    if (!threaded) {
      return *why = "a reply answers a message this address sent the site in the last 30 days: in_reply_to must be "
                    "its Message-ID",
             std::nullopt;
    }
    return c;
  }
  if (category == "transactional") {
    if (c.relationship.empty()) {
      return *why = "transactional mail goes to an account of this site or someone who wrote to it in the last 90 "
                    "days; for an address someone just typed, use confirm",
             std::nullopt;
    }
    return c;
  }
  if (category == "confirm") {
    if (!person_present && !at_delivery) {
      return *why = "confirm mail is sent from an http route, while the person who typed the address is there",
             std::nullopt;
    }
    int64_t now = base::now_unix_ms();
    if (consent_->asked_since(tenant, hash, "confirm", now - kDay) >= 1 ||
        consent_->asked_since(tenant, hash, "confirm", now - 30 * kDay) >= 3) {
      return *why = "confirm mail is at most 1 a day and 3 in 30 days to one address", std::nullopt;
    }
    return c;
  }
  if (category == "outreach") {
    if (!need_postal()) return std::nullopt;
    auto o = consent_->outreach(tenant, hash);
    if (o && o->opted_out) return *why = to + " asked for no more outreach from this site", std::nullopt;
    int64_t now = base::now_unix_ms();
    if (o && o->last_ms > now - mail::consent::kOutreachEveryMs) {
      return *why = "outreach is one mail per address in 30 days, unless they reply (then answer with category reply); " +
                        to + " had one on " + base::format_utc_ms(o->last_ms).substr(0, 10),
             std::nullopt;
    }
    if (consent_->outreach_since(tenant, now - kDay) >= cfg->mail.outreach_daily_max) {
      return *why = "the tenant sent its " + std::to_string(cfg->mail.outreach_daily_max) +
                        " outreach mails for the day (config mail.outreach-daily-max)",
             std::nullopt;
    }
    return c;
  }
  if (category == "list") {
    if (!need_postal()) return std::nullopt;
    if (list.empty()) return *why = "list mail names its list: {\"category\": \"list\", \"list\": \"digest\"}", std::nullopt;
    auto state = consent_->list_state(tenant, hash, list);
    if (!state || state->state != "in") {
      return *why = to + " isn't subscribed to " + list + ": ask with the mail service's subscribe", std::nullopt;
    }
    return c;
  }
  return *why = "category is reply, transactional, confirm, outreach, or list", std::nullopt;
}

std::string Host::footer_reason(const MailCheck& c, const std::string& site, const std::string& list_title) const {
  if (c.category == "reply") return "You're getting this because you wrote to " + site + ".";
  if (c.category == "transactional") {
    return c.relationship == "account" ? "You're getting this because you have an account on " + site + "."
                                       : "You're getting this because you wrote to " + site + ".";
  }
  if (c.category == "confirm") {
    return "You're getting this because someone entered this address on " + site +
           ". If it wasn't you, ignore it: nothing more will come.";
  }
  if (c.category == "outreach") return site + " wrote to you once. It won't again unless you reply.";
  if (c.category == "list") return "You're getting this because you subscribed to " + list_title + " on " + site + ".";
  if (c.category == "list_confirm") {
    return "You're getting this because someone asked " + site +
           " to subscribe this address. Nothing more comes unless you confirm.";
  }
  return "";
}

void Host::add_footer(std::string* text, std::string* html, const std::string& why, const std::string& postal,
                      const std::string& link_label, const std::string& link) {
  std::string t = "\n\n--\n" + why + "\n";
  if (!postal.empty()) t += postal + "\n";
  t += link_label + ": " + link + "\n";
  *text += t;
  if (html->empty()) return;
  std::string h =
      "<div style=\"max-width:560px;margin:0 auto;padding:8px 16px 24px;font:12px/1.5 -apple-system,BlinkMacSystemFont,"
      "'Segoe UI',Roboto,Helvetica,Arial,sans-serif;color:#71717a;text-align:center;\"><p style=\"margin:0 0 4px;\">" +
      html_escape(why) + "</p>" +
      (postal.empty() ? std::string() : "<p style=\"margin:0 0 4px;\">" + html_escape(postal) + "</p>") +
      "<p style=\"margin:0;\"><a href=\"" + html_escape(link) + "\" style=\"color:#71717a;\">" + html_escape(link_label) +
      "</a></p></div>";
  auto at = html->rfind("</body>");
  if (at == std::string::npos) {
    *html += h;
  } else {
    html->insert(at, h);
  }
}

// /goober/mail/$tenant/$hash/$mac[/$scope], and the links mailed before 029,
// /goober/unsubscribe/$tenant/$hash/$mac. GET shows; a POST only ever reduces mail.
void Host::handle_unsubscribe(const Request& req, const Finish& finish, const std::string& path,
                              const std::string& tenant) {
  std::string request_id = req.header("goober-request-id");
  bool old = path.starts_with("/goober/unsubscribe/");
  std::string rest = path.substr(old ? std::string("/goober/unsubscribe/").size() : std::string("/goober/mail/").size());
  std::vector<std::string> parts;
  for (size_t i = 0; i <= rest.size();) {
    size_t j = rest.find('/', i);
    if (j == std::string::npos) j = rest.size();
    parts.push_back(rest.substr(i, j - i));
    i = j + 1;
  }
  if (parts.size() < 3 || parts.size() > 4) return finish(error_page(tenant, 404, request_id), R"({"unsubscribe":"bad link"})");
  const std::string &t = parts[0], &hash = parts[1], &mac = parts[2];
  std::string scope = parts.size() == 4 ? parts[3] : "";
  std::string want = base::hmac_sha256_hex(reply_key(), "goober-unsubscribe\n" + t + "\n" + hash).substr(0, 32);
  if (t != tenant || !is_hex(hash, 32) || !is_hex(mac, 32) || !same(mac, want)) {
    return finish(error_page(tenant, 404, request_id), R"({"unsubscribe":"bad link"})");
  }
  std::string list = scope.starts_with("list.") ? scope.substr(5) : "";
  if (!scope.empty() && list.empty()) return finish(error_page(tenant, 404, request_id), R"({"unsubscribe":"bad link"})");
  auto lists = tenant_lists(tenant);
  std::string site = site_of(tenant);
  int64_t now = base::now_unix_ms();
  auto form = router::parse_query(req.body);
  auto title_of = [&](const std::string& name) {
    auto it = lists.find(name);
    return it == lists.end() ? name : it->second.first.title;
  };
  auto self = [&]() { return path; };
  if (req.method == "post") {
    // The one-click POST (RFC 8058) comes from the person's mail provider, with no cookie and
    // no Origin of ours: the link's MAC is the whole authority, and it only ever reduces mail.
    bool everything = form.count("all") > 0 || (list.empty() && form.count("leave") == 0);
    std::string leave = form.count("leave") ? form["leave"] : list;
    bj::object evidence{{"request_id", request_id}, {"via", form.count("List-Unsubscribe") ? "one_click" : "page"}};
    if (everything) {
      consent_->add_block(tenant, hash, "unsubscribed", evidence, now);
      return finish(identity_page(tenant, 200, "Unsubscribed",
                                  "<h1>You're unsubscribed</h1><p>" + html_escape(site) +
                                      " won't email you again: no lists, no invites, nothing from its boxes. Sign-in "
                                      "codes you ask for still come.</p><p class=\"muted\">Changed your mind? Sign in "
                                      "on the site and allow mail again on your account page.</p>"),
                    R"({"unsubscribe":"done","scope":"all"})");
    }
    if (!base::is_valid_name(leave)) return finish(error_page(tenant, 400, request_id), R"({"unsubscribe":"bad list"})");
    consent_->opt_out(tenant, hash, leave, "link", evidence, now);
    return finish(identity_page(tenant, 200, "Unsubscribed",
                                "<h1>You've left " + html_escape(title_of(leave)) + "</h1><p>No more of it will come. "
                                "Other mail from " + html_escape(site) + " is unchanged.</p><form method=\"post\" action=\"" +
                                    html_escape(self()) + "\"><button name=\"all\" value=\"1\" class=\"secondary\">Stop all "
                                    "mail from " + html_escape(site) + "</button></form>"),
                  json::dump(bj::object{{"unsubscribe", "done"}, {"scope", "list"}, {"list", leave}}));
  }
  if (req.method != "get" && req.method != "head") {
    return finish(error_page(tenant, 405, request_id), R"({"unsubscribe":"method"})");
  }
  // A GET never changes anything: mail scanners open every link in a message.
  std::string body;
  if (consent_->block(tenant, hash)) {
    body = "<h1>You're unsubscribed</h1><p>" + html_escape(site) + " doesn't email you. To allow it again, sign in on "
           "the site and use your account page.</p>";
    return finish(identity_page(tenant, 200, "Mail preferences", body), R"({"unsubscribe":"blocked"})");
  }
  if (!list.empty()) {
    body = "<h1>Leave " + html_escape(title_of(list)) + "?</h1><p>You won't get any more of it.</p><form method=\"post\" "
           "action=\"" + html_escape(self()) + "\"><button name=\"leave\" value=\"" + html_escape(list) + "\">Leave</button></form>";
  } else {
    body = "<h1>Stop these emails?</h1><p>You won't get any more mail from " + html_escape(site) +
           "'s boxes, lists, or invites. Sign-in codes you ask for still come.</p><form method=\"post\" action=\"" +
           html_escape(self()) + "\"><button name=\"List-Unsubscribe\" value=\"One-Click\">Unsubscribe</button></form>";
  }
  std::string mine;
  for (const auto& s : consent_->lists(tenant, hash)) {
    if (s.state != "in") continue;
    mine += "<li>" + html_escape(title_of(s.list)) + " <form method=\"post\" action=\"" + html_escape(self()) +
            "\"><button class=\"small secondary\" name=\"leave\" value=\"" + html_escape(s.list) + "\">Leave</button></form></li>";
  }
  if (!mine.empty()) body += "<p><strong>Your lists</strong></p><ul>" + mine + "</ul>";
  if (!list.empty()) {
    body += "<form method=\"post\" action=\"" + html_escape(self()) + "\"><button class=\"secondary\" name=\"all\" "
            "value=\"1\">Stop all mail from " + html_escape(site) + "</button></form>";
  }
  body += "<p class=\"muted\">Joining a list takes signing in, on your <a href=\"/identity/account\">account page</a>.</p>";
  return finish(identity_page(tenant, 200, "Mail preferences", body), R"({"unsubscribe":"asked"})");
}

// /goober/list/$tenant/$hash/$list/$mac: the confirm link of a list-confirm mail (double opt-in).
void Host::handle_list_confirm(const Request& req, const Finish& finish, const std::string& path,
                               const std::string& tenant) {
  std::string request_id = req.header("goober-request-id");
  std::string rest = path.substr(std::string("/goober/list/").size());
  std::vector<std::string> parts;
  for (size_t i = 0; i <= rest.size();) {
    size_t j = rest.find('/', i);
    if (j == std::string::npos) j = rest.size();
    parts.push_back(rest.substr(i, j - i));
    i = j + 1;
  }
  if (parts.size() != 4) return finish(error_page(tenant, 404, request_id), R"({"list":"bad link"})");
  const std::string &t = parts[0], &hash = parts[1], &list = parts[2], &mac = parts[3];
  std::string want =
      base::hmac_sha256_hex(reply_key(), "goober-list-confirm\n" + t + "\n" + hash + "\n" + list).substr(0, 32);
  if (t != tenant || !is_hex(hash, 32) || !base::is_valid_name(list) || !is_hex(mac, 32) || !same(mac, want)) {
    return finish(error_page(tenant, 404, request_id), R"({"list":"bad link"})");
  }
  auto lists = tenant_lists(tenant);
  auto it = lists.find(list);
  if (it == lists.end()) {
    return finish(identity_page(tenant, 410, "Gone", "<h1>That list is gone</h1><p>Nothing was changed.</p>"),
                  R"({"list":"gone"})");
  }
  std::string site = site_of(tenant);
  const auto& l = it->second.first;
  if (req.method == "post") {
    if (!same_origin(req, origin_of(req))) return finish(error_page(tenant, 403, request_id), R"({"list":"cross-origin"})");
    std::string why;
    bj::object evidence{{"request_id", request_id}, {"page", "list-confirm"}, {"title", l.title}, {"about", l.about}};
    if (!consent_->opt_in(tenant, hash, list, "confirmed", evidence, base::now_unix_ms(), &why)) {
      return finish(identity_page(tenant, 409, "Not subscribed", "<h1>Not subscribed</h1><p>" + html_escape(why) + "</p>"),
                    R"({"list":"refused"})");
    }
    return finish(identity_page(tenant, 200, "Subscribed",
                                "<h1>You're subscribed</h1><p>You'll get " + html_escape(l.title) + " from " +
                                    html_escape(site) + ". Every one has a link to leave.</p>"),
                  json::dump(bj::object{{"list", "confirmed"}, {"name", list}}));
  }
  if (req.method != "get" && req.method != "head") return finish(error_page(tenant, 405, request_id), R"({"list":"method"})");
  return finish(identity_page(tenant, 200, "Subscribe?",
                              "<h1>Subscribe to " + html_escape(l.title) + "?</h1><p>" + html_escape(l.about) +
                                  "</p><p class=\"muted\">From " + html_escape(site) +
                                  ". Every mail has a link to leave.</p><form method=\"post\"><button>Yes, subscribe me</button>"
                                  "</form><p class=\"muted\">If you didn't ask for this, close this page: nothing happens.</p>"),
                R"({"list":"asked"})");
}

std::string Host::mail_preferences_call(CallContext& ctx, const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  if (!request.is_object()) return fail("preferences takes {email}");
  std::string to = base::to_lower(field(request.as_object(), "email"));
  if (!plain_email(to)) return fail("email is one address");
  std::string hash = address_hash(to);
  bj::object lists;
  for (const auto& [name, l] : tenant_lists(ctx.tenant)) {
    auto s = consent_->list_state(ctx.tenant, hash, name);
    lists[name] = s ? s->state : "none";
  }
  bool account = identity_->proved(ctx.tenant, to);
  std::string relationship = account ? "account" : wrote_in(ctx.tenant, to, 90 * kDay) ? "wrote_in" : "none";
  bj::object outreach{{"state", "allowed"}};
  if (auto o = consent_->outreach(ctx.tenant, hash)) {
    if (o->opted_out) {
      outreach["state"] = "opted_out";
    } else if (o->last_ms > base::now_unix_ms() - mail::consent::kOutreachEveryMs) {
      outreach["state"] = "used";
      outreach["at"] = base::format_utc_ms(o->last_ms);
    }
  }
  return json::dump(bj::object{{"email", to},
                               {"blocked", consent_->block_of(ctx.tenant, to).has_value()},
                               {"lists", std::move(lists)},
                               {"relationship", relationship},
                               {"outreach", std::move(outreach)}});
}

std::string Host::mail_subscribe_call(CallContext& ctx, const manifest::ExternalService& svc, const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  if (!request.is_object()) return fail("subscribe takes {email, list}");
  const auto& r = request.as_object();
  std::string to = base::to_lower(field(r, "email")), list = field(r, "list");
  if (!plain_email(to)) return fail("email is one address");
  auto mine = svc.mail_lists.find(list);
  if (mine == svc.mail_lists.end()) return fail("list " + list + " isn't one this mail service declares");
  auto cfg = config_.get();
  const std::string& postal = cfg->tenant(ctx.tenant).postal_address;
  if (postal.empty()) {
    return fail("list mail needs the tenant's postal address: set tenants." + ctx.tenant + ".postal-address in the host config");
  }
  std::string hash = address_hash(to);
  if (consent_->block_of(ctx.tenant, to)) return fail("blocked: " + to + " asked for no more mail from this site");
  if (auto s = consent_->list_state(ctx.tenant, hash, list); s && s->state == "in") {
    return json::dump(bj::object{{"accepted", false}, {"already", true}});
  } else if (s && s->state == "out") {
    // Leaving is final until the person joins again themselves: signed in as that address.
    bool themselves = ctx.who && !ctx.who->host &&
                      mail::consent::canonical_address(ctx.who->user.email) == mail::consent::canonical_address(to);
    if (!themselves) return fail(to + " left " + list + "; only they can join again, signed in on the site");
  }
  int64_t now = base::now_unix_ms();
  if (consent_->asked_since(ctx.tenant, hash, "list-confirm:" + list, now - 30 * kDay) > 0) {
    return fail("a confirmation for " + list + " went to " + to + " in the last 30 days; one is enough");
  }
  std::string site = site_of(ctx.tenant);
  std::string mac = base::hmac_sha256_hex(reply_key(), "goober-list-confirm\n" + ctx.tenant + "\n" + hash + "\n" + list).substr(0, 32);
  std::string base = cfg->local || site.empty() ? cfg->base_url() : "https://" + site;
  std::string confirm = base + "/goober/list/" + ctx.tenant + "/" + hash + "/" + list + "/" + mac;
  bj::object vars{{"site", site}, {"email", to}, {"list_title", mine->second.title}, {"list_about", mine->second.about},
                  {"confirm_url", confirm}};
  std::string err;
  auto rendered = mail_templates_->render(ctx.tenant, "list-confirm", vars, &err);
  if (!rendered) return fail("the list-confirm template: " + err);
  if (rendered->text.find(confirm) == std::string::npos) {
    // A tenant's template that drops the link can't be confirmed: the host's own goes instead.
    rendered = mail::templates::render_with(mail::templates::builtin("list-confirm")->defaults,
                                            *mail_templates_->get(ctx.tenant, "layout"), vars, &err);
    if (!rendered) return fail("the list-confirm template: " + err);
  }
  std::string text = rendered->text, html = rendered->html;
  MailCheck c{"list_confirm", list, ""};
  std::string link = mail_link(site, ctx.tenant, to, "");
  add_footer(&text, &html, footer_reason(c, site, mine->second.title), postal, "Unsubscribe", link);
  OutboxElement e;
  e.service = svc.name;
  e.method = "subscribe";
  e.verb = "MAIL";
  e.url = "mail:" + to;
  e.body = json::dump(bj::object{{"to", to},
                                 {"subject", rendered->subject},
                                 {"text", base::valid_utf8(text)},
                                 {"html", base::valid_utf8(html)},
                                 {"from_name", svc.mail_from_name},
                                 {"from_address", svc.mail_from},
                                 {"list_unsubscribe", "<" + link + ">"},
                                 {"category", "list_confirm"},
                                 {"list", list}});
  e.idempotency_key = base::sha256_hex("goober-outbox\n" + ctx.request_id + "\n" + std::to_string(ctx.pending.size())).substr(0, 32);
  e.attempts = 6;
  e.backoff_ms = 30000;
  e.within_ms = kDay;
  std::string key = e.idempotency_key;
  ctx.pending.push_back(std::move(e));
  // Counted when asked, so a second ask before delivery is refused too (a request that rolls back
  // still counts: the limit errs toward fewer mails).
  consent_->asked(ctx.tenant, hash, "list-confirm:" + list, now);
  return json::dump(bj::object{{"accepted", true}, {"key", key}});
}

std::string Host::account_mail_section(const std::string& tenant, const std::string& email) {
  std::string hash = address_hash(email);
  std::string site = site_of(tenant);
  std::string out = "<p><strong>Mail</strong></p>";
  if (auto b = consent_->block_of(tenant, email)) {
    out += "<p>You asked " + html_escape(site) + " not to email you on " + base::format_utc_ms(b->at_ms).substr(0, 10) +
           ". Nothing comes but the sign-in codes you ask for.</p><form method=\"post\" action=\"/identity/mail/allow\">"
           "<label class=\"check\"><input type=\"checkbox\" name=\"statement\" value=\"I'm asking " + html_escape(site) +
           " to email me again.\" required> I'm asking " + html_escape(site) + " to email me again.</label>"
           "<button class=\"secondary\">Allow mail again</button></form>";
    return out;
  }
  auto lists = tenant_lists(tenant);
  if (lists.empty()) {
    out += "<p class=\"muted\">" + html_escape(site) + " has no lists to join.</p>";
  } else {
    out += "<ul>";
    for (const auto& [name, l] : lists) {
      auto s = consent_->list_state(tenant, hash, name);
      bool in = s && s->state == "in";
      out += "<li><strong>" + html_escape(l.first.title) + "</strong> <span class=\"muted\">" + html_escape(l.first.about) +
             "</span> <form method=\"post\" action=\"/identity/mail/" + std::string(in ? "leave" : "join") +
             "\"><input type=\"hidden\" name=\"list\" value=\"" + html_escape(name) + "\"><button class=\"small" +
             (in ? " secondary\">Leave" : "\">Join") + "</button></form></li>";
    }
    out += "</ul>";
  }
  out += "<form method=\"post\" action=\"/identity/mail/stop\"><button class=\"small secondary\">Stop all mail from " +
         html_escape(site) + "</button></form>";
  return out;
}

bool Host::account_mail_post(const std::string& tenant, const SignedIn& who, const std::string& path,
                             const std::map<std::string, std::string>& form, const std::string& request_id,
                             std::string* why) {
  std::string hash = address_hash(who.user.email);
  int64_t now = base::now_unix_ms();
  auto get = [&](const char* k) {
    auto it = form.find(k);
    return it == form.end() ? std::string() : it->second;
  };
  bj::object evidence{{"request_id", request_id}, {"user_id", std::to_string(who.user.user_id)}, {"page", "account"}};
  if (path == "/identity/mail/allow") {
    // The person, signed in (the address proved by a code), lifts their own block (029 § 6). A
    // block covers every spelling of the mailbox (alice+x@, a.lice@ at gmail), but only the
    // mailbox's own address lifts it: on other providers alice+x@ may be someone else.
    std::string legacy = mail::consent::legacy_address_hash(who.user.email);
    bool own = mail::consent::canonical_address(who.user.email) == base::to_lower(who.user.email);
    bool lifted = own && consent_->lift(tenant, hash, "person", get("statement"), request_id, now, why).has_value();
    if (legacy != hash) {
      std::string w2;
      lifted = consent_->lift(tenant, legacy, "person", get("statement"), request_id, now, &w2).has_value() || lifted;
    }
    if (!own && !lifted) *why = "sign in with " + mail::consent::canonical_address(who.user.email) + " to allow mail again";
    return lifted;
  }
  if (path == "/identity/mail/stop") return consent_->add_block(tenant, hash, "unsubscribed", evidence, now) || true;
  std::string list = get("list");
  auto lists = tenant_lists(tenant);
  auto it = lists.find(list);
  if (it == lists.end()) return *why = "no such list", false;
  if (path == "/identity/mail/join") {
    evidence["title"] = it->second.first.title;
    evidence["about"] = it->second.first.about;
    return consent_->opt_in(tenant, hash, list, "account", evidence, now, why);
  }
  if (path == "/identity/mail/leave") {
    consent_->opt_out(tenant, hash, list, "account", evidence, now);
    return true;
  }
  return *why = "no such mail action", false;
}

std::pair<int, bj::object> Host::panel_unblock(const SignedIn& who, const bj::object& args, const std::string& request_id) {
  auto has = [&](const char* role) { return std::find(who.roles.begin(), who.roles.end(), role) != who.roles.end(); };
  if (!has("host:owner") && !has("host:operator")) return {403, bj::object{{"error", "an observer can't lift a block"}}};
  const auto* c = args.if_contains("confirm");
  if (!c || !c->is_bool() || !c->as_bool()) return {400, bj::object{{"error", "confirm the change: {\"confirm\": true}"}}};
  std::string tenant = field(args, "tenant"), email = base::to_lower(field(args, "email")), statement = field(args, "statement");
  auto scope = operator_scope("panel:" + who.user.email);
  if (!scope.all && !scope.tenants.count(tenant)) return {403, bj::object{{"error", "outside your scope"}}};
  bool known = false;
  for (const auto& [d, e] : config_.get()->domains) known |= e.tenant == tenant;
  if (!known) return {400, bj::object{{"error", "no domain in the host config serves tenant " + tenant}}};
  if (email.find('@') == std::string::npos) return {400, bj::object{{"error", "email is one address"}}};
  std::string why;
  auto lifted = consent_->lift(tenant, address_hash(email), "operator:" + who.user.email, statement, request_id,
                               base::now_unix_ms(), &why);
  if (std::string legacy = mail::consent::legacy_address_hash(email); legacy != address_hash(email)) {
    std::string w2;
    if (auto old = consent_->lift(tenant, legacy, "operator:" + who.user.email, statement, request_id, base::now_unix_ms(), &w2)) {
      lifted = old;
    }
  }
  if (!lifted) {
    if (why == "that address isn't blocked") return {200, bj::object{{"lifted", false}}};
    return {400, bj::object{{"error", why}}};
  }
  if (changes_) {
    try {
      changes_->record_action(operations::Action{base::now_unix_ms(), "panel:" + who.user.email, "mail_block_lift", request_id,
                                                 bj::object{{"tenant", tenant}, {"how", lifted->how}}, false});
    } catch (const std::exception& e) {
      std::cerr << "goober-host: change log: " << e.what() << "\n";
    }
  }
  return {200, bj::object{{"lifted", true}, {"blocked_at", lifted->at_ms}, {"how", lifted->how}}};
}

void Host::register_consent_tools() {
  using mcp::object_schema;
  using mcp::string_property;
  auto known = [this](const std::string& tenant) {
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == tenant) return true;
    }
    return false;
  };
  mcp_->add_tool(
      {"mail_preferences",
       "What a tenant may mail one address, and why: blocked (it declined an invite or unsubscribed; only the person "
       "or an operator in the panel lifts it), each list in, out, or none (with how and when), the relationship "
       "(account, wrote_in, none), outreach (allowed, used, opted_out), and the history of every change.",
       object_schema({{"tenant", string_property("A tenant name from the host config.")},
                      {"email", string_property("The address.")}},
                     {"tenant", "email"}),
       [this, known](const bj::object& args) {
         std::string tenant = field(args, "tenant"), email = base::to_lower(field(args, "email"));
         if (!known(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
         if (email.find('@') == std::string::npos) return ToolResult::fail("email is one address");
         std::string hash = address_hash(email);
         bj::value block = nullptr;
         if (auto b = consent_->block_of(tenant, email)) {
           block = bj::object{{"at", base::format_utc_ms(b->at_ms)}, {"how", b->how}};
         }
         bj::object lists;
         for (const auto& s : consent_->lists(tenant, hash)) {
           lists[s.list] = bj::object{{"state", s.state}, {"how", s.how}, {"at", base::format_utc_ms(s.at_ms)}};
         }
         bool account = identity_->proved(tenant, email);
         std::string relationship = account ? "account" : wrote_in(tenant, email, 90 * kDay) ? "wrote_in" : "none";
         bj::object outreach{{"state", "allowed"}};
         if (auto o = consent_->outreach(tenant, hash)) {
           outreach = bj::object{{"state", o->opted_out ? "opted_out" : "used"},
                                 {"count", o->count},
                                 {"last", base::format_utc_ms(o->last_ms)}};
         }
         return ToolResult::ok(bj::object{{"tenant", tenant},
                                          {"email", email},
                                          {"blocked", std::move(block)},
                                          {"lists", std::move(lists)},
                                          {"relationship", relationship},
                                          {"outreach", std::move(outreach)},
                                          {"history", consent_->history(tenant, hash)}});
       }});
  mcp_->add_tool(
      {"mail_consent_record",
       "Records that a person agreed elsewhere (in person, on paper, by phone) to a tenant's list, with the "
       "statement in your words: it's kept as evidence, shown as an operator's record. It never re-subscribes "
       "someone who left the list, and never reaches a blocked address.",
       object_schema({{"tenant", string_property("A tenant name from the host config.")},
                      {"email", string_property("The address.")},
                      {"list", string_property("A list a box of the tenant declares.")},
                      {"statement", string_property("How they agreed, at least 20 characters: \"Signed up at the 11/2 meetup\".")}},
                     {"tenant", "email", "list", "statement"}),
       [this, known](const bj::object& args) {
         std::string tenant = field(args, "tenant"), email = base::to_lower(field(args, "email")), list = field(args, "list"),
                     statement = field(args, "statement");
         if (!known(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
         if (email.find('@') == std::string::npos) return ToolResult::fail("email is one address");
         if (!tenant_lists(tenant).count(list)) return ToolResult::fail("no box of " + tenant + " declares a list " + list);
         if (statement.size() < mail::consent::kMinStatement || statement.size() > 1000) {
           return ToolResult::fail("statement says how they agreed, 20 to 1000 characters");
         }
         std::string why;
         if (!consent_->opt_in(tenant, address_hash(email), list, "operator",
                               bj::object{{"statement", statement}, {"by", current_operator()}}, base::now_unix_ms(), &why)) {
           return ToolResult::fail(why);
         }
         return ToolResult::ok(bj::object{{"tenant", tenant}, {"email", email}, {"list", list}, {"state", "in"}});
       }});
  mcp_->add_tool({"mail_lists",
                  "A tenant's lists: each name, title, about, the box that declares it, and how many are subscribed.",
                  object_schema({{"tenant", string_property("A tenant name from the host config.")}}, {"tenant"}),
                  [this, known](const bj::object& args) {
                    std::string tenant = field(args, "tenant");
                    if (!known(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
                    bj::array out;
                    for (const auto& [name, l] : tenant_lists(tenant)) {
                      out.push_back(bj::object{{"list", name},
                                               {"title", l.first.title},
                                               {"about", l.first.about},
                                               {"box", l.second},
                                               {"subscribed", consent_->count_in(tenant, name)}});
                    }
                    return ToolResult::ok(bj::object{{"tenant", tenant}, {"lists", std::move(out)}});
                  }});
}

}  // namespace goober::host
