// Invites (design/machine/029.Invites.md § 2): the host keeps the record, the template, and the
// limits. An operator invites with invite_send; a box asks its invites service, after its request
// commits (the outbox, verb INVITE). The links are capabilities under the host's reply key; a
// GET never changes anything, accepting takes a sign-in code, and declining is always possible.

#include <algorithm>
#include <iostream>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/router/path.h"
#include "goober/sqlite/sqlite.h"
#include "http_util.h"
#include "mailer.h"
#include "network.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using base::html_escape;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;
using mail::consent::address_hash;

namespace {

constexpr int64_t kDay = 24LL * 3600 * 1000;

std::string field(const bj::object& o, const char* k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

std::vector<std::string> strings(const bj::object& o, const char* k, bool* ok) {
  std::vector<std::string> out;
  auto* v = o.if_contains(k);
  if (!v) return out;
  if (!v->is_array() || v->as_array().size() > 20) {
    *ok = false;
    return out;
  }
  for (const auto& e : v->as_array()) {
    if (!e.is_string()) {
      *ok = false;
      return out;
    }
    out.push_back(std::string(e.as_string()));
  }
  return out;
}

bool same(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
  return d == 0;
}

// What an invite asks of someone, for the page and the mail: role names as words.
std::string role_words(const std::vector<std::string>& roles) {
  std::string out;
  for (size_t i = 0; i < roles.size(); ++i) out += (i ? ", " : "") + roles[i];
  return out;
}

}  // namespace

std::string Host::invite_mac(const std::string& tenant, int64_t id, const std::string& email) {
  return base::hmac_sha256_hex(reply_key(), "goober-invite\n" + tenant + "\n" + std::to_string(id) + "\n" + email)
      .substr(0, 32);
}

std::string Host::invite_url(const std::string& tenant, const identity::Invite& inv, bool decline) {
  auto cfg = config_.get();
  std::string site = site_of(tenant);
  std::string base = cfg->local || site.empty() || site == "localhost" ? cfg->base_url() : "https://" + site;
  return base + "/identity/invite/" + std::to_string(inv.id) + "/" + invite_mac(tenant, inv.id, inv.email) +
         (decline ? "/decline" : "");
}

std::optional<std::pair<std::string, std::string>> Host::terms_now(const std::string& tenant) {
  const auto& path = config_.get()->tenant(tenant).terms;
  if (path.empty()) return std::nullopt;
  site::Visit v;
  v.tenant = tenant;
  v.path = path;
  v.accept = "text/html";
  auto r = site_->visit(v);
  std::string bytes;
  if (r.status == 200) {
    auto f = r.file.empty() ? std::nullopt : content_->get(tenant, r.file);
    bytes = f ? f->data : r.body;
  }
  return std::pair{path, bytes.empty() ? std::string("missing") : base::sha256_hex(bytes)};
}

bool Host::send_invite_mail(const std::string& tenant, const identity::Invite& inv, bool reminder, std::string* why) {
  auto cfg = config_.get();
  if (consent_->block_of(tenant, inv.email)) return *why = "blocked", false;
  // Invites count against the tenant's mail for the day, in the same ledger as box mail.
  std::lock_guard lock(sent_mu_);
  fs::create_directories(cfg->storage / "mail");
  sqlite::Db ledger(cfg->storage / "mail" / "sent.db");
  ledger.exec("CREATE TABLE IF NOT EXISTS sent (tenant TEXT NOT NULL, address TEXT NOT NULL, at_ms INTEGER NOT NULL)");
  int64_t now = base::now_unix_ms();
  sqlite::Stmt count(ledger, "SELECT count(*) FROM sent WHERE tenant = ? AND at_ms > ?");
  count.bind(1, tenant).bind(2, now - kDay);
  count.step();
  if (count.int64(0) >= cfg->mail.daily_max) {
    return *why = "the tenant has sent its " + std::to_string(cfg->mail.daily_max) + " mails for the day (config mail.daily-max)",
           false;
  }
  const auto& tc = cfg->tenant(tenant);
  std::string site = site_of(tenant);
  std::string accept = invite_url(tenant, inv, false), decline = invite_url(tenant, inv, true);
  std::string base = cfg->local || site.empty() ? cfg->base_url() : "https://" + site;
  std::string inviter = inv.inviter_kind == "site"       ? site
                        : inv.inviter_kind == "operator" ? (inv.inviter_name.empty() ? "The team at " + site : inv.inviter_name)
                        : inv.inviter_name.empty()       ? inv.inviter_email
                                                         : inv.inviter_name + " (" + inv.inviter_email + ")";
  bj::array roles, lists;
  for (const auto& r : inv.roles) roles.push_back(bj::object{{"role", r}});
  auto known = tenant_lists(tenant);
  for (const auto& l : inv.lists) {
    auto it = known.find(l);
    if (it != known.end()) lists.push_back(bj::object{{"title", it->second.first.title}, {"about", it->second.first.about}});
  }
  bj::object vars{{"site", site},
                  {"email", inv.email},
                  {"inviter", inviter},
                  {"inviter_email", inv.inviter_email},
                  {"note", inv.note},
                  {"has_roles", !roles.empty()},
                  {"has_lists", !lists.empty()},
                  {"roles", std::move(roles)},
                  {"lists", std::move(lists)},
                  {"terms_url", tc.terms.empty() ? "" : base + tc.terms},
                  {"privacy_url", tc.privacy.empty() ? "" : base + tc.privacy},
                  {"accept_url", accept},
                  {"decline_url", decline},
                  {"postal_address", tc.postal_address},
                  {"expires", base::format_utc_ms(inv.expires_at).substr(0, 10)},
                  {"reminder", reminder}};
  std::string err;
  auto rendered = mail_templates_->render(tenant, "invite", vars, &err);
  auto has_links = [&](const mail::templates::Rendered& r) {
    return r.text.find(accept) != std::string::npos && r.text.find(decline) != std::string::npos &&
           (r.html.empty() || (r.html.find(accept) != std::string::npos && r.html.find(decline) != std::string::npos));
  };
  if (!rendered || !has_links(*rendered)) {
    // A tenant's template that drops either link isn't sent: the host's own goes instead (§ 2.3).
    if (rendered) std::cerr << "goober-host: " << tenant << "'s invite template drops a link; the default went instead\n";
    auto layout = mail_templates_->get(tenant, "layout");
    rendered = mail::templates::render_with(mail::templates::builtin("invite")->defaults, *layout, vars, &err);
    if (!rendered) return *why = "the invite template: " + err, false;
  }
  std::string text = rendered->text, html = rendered->html;
  add_footer(&text, &html,
             inviter + " invited you to " + site + ". " +
                 (reminder ? "This is the one reminder: nothing more comes." : "You'll get one reminder, then nothing."),
             tc.postal_address, "Don't invite me again", decline);
  identity::Mail m{inv.email, rendered->subject, text};
  m.html = html;
  m.from_name = site;
  m.list_unsubscribe = "<" + decline + ">";
  m.tenant = tenant;
  mail_->send(std::move(m));
  sqlite::Stmt add(ledger, "INSERT INTO sent (tenant, address, at_ms) VALUES (?, ?, ?)");
  add.bind(1, tenant).bind(2, address_hash(inv.email)).bind(3, now).run();
  return true;
}

std::optional<identity::Invite> Host::make_invite(const std::string& tenant, identity::Invite inv, std::string* code,
                                                  std::string* why) {
  auto cfg = config_.get();
  const auto& tc = cfg->tenant(tenant);
  inv.email = base::to_lower(inv.email);
  if (tc.postal_address.empty()) {
    *code = "no_postal_address";
    return *why = "invites need the tenant's postal address: set tenants." + tenant + ".postal-address in the host config",
           std::nullopt;
  }
  if (consent_->block_of(tenant, inv.email)) {
    return *code = "blocked", *why = inv.email + " asked for no invites or mail from this site", std::nullopt;
  }
  auto lists = tenant_lists(tenant);
  for (const auto& l : inv.lists) {
    if (!lists.count(l)) return *code = "bad_input", *why = "no box of " + tenant + " declares a list " + l, std::nullopt;
  }
  int64_t now = base::now_unix_ms();
  if (identity_->invites_since(tenant, now - kDay) >= tc.invites.daily_max) {
    *code = "limit";
    return *why = "the tenant made its " + std::to_string(tc.invites.daily_max) + " invites for the day (config tenants." +
                  tenant + ".invites.daily-max)",
           std::nullopt;
  }
  if (inv.inviter_kind == "person" && identity_->invites_since(tenant, now - kDay, inv.inviter_email) >= tc.invites.per_inviter_daily) {
    *code = "limit";
    return *why = inv.inviter_email + " made their " + std::to_string(tc.invites.per_inviter_daily) + " invites for the day",
           std::nullopt;
  }
  if (auto t = terms_now(tenant)) inv.terms_sha256 = t->second;
  auto made = identity_->create_invite(tenant, inv, now, code, why);
  if (!made) return std::nullopt;
  if (!send_invite_mail(tenant, *made, false, why)) {
    std::string ignored;
    identity_->revoke_invite(tenant, made->id, "unsent", now, &ignored);
    *code = "unsent";
    return std::nullopt;
  }
  return made;
}

std::optional<std::string> Host::accept_invite_for(const std::string& tenant, const identity::Invite& inv,
                                                   const identity::User& user, const std::string& how,
                                                   const std::string& request_id, std::string* why) {
  auto cfg = config_.get();
  const auto& allowed_roles = cfg->tenant(tenant).invites.roles;
  // An operator's invite grants what it says; a box's only what the host config still allows.
  auto allowed = [&](const std::string& role) {
    return inv.box.empty() || std::find(allowed_roles.begin(), allowed_roles.end(), role) != allowed_roles.end();
  };
  auto accepted = identity_->accept_invite(tenant, inv.id, user.user_id, how, allowed, base::now_unix_ms(), why);
  if (!accepted) return std::nullopt;
  int64_t now = base::now_unix_ms();
  if (auto t = terms_now(tenant)) identity_->accept_terms(tenant, user.user_id, t->first, t->second, "invite", now);
  auto lists = tenant_lists(tenant);
  std::string hash = address_hash(inv.email);
  for (const auto& l : inv.lists) {
    auto it = lists.find(l);
    if (it == lists.end()) continue;
    std::string ignored;
    consent_->opt_in(tenant, hash, l, "invite",
                     bj::object{{"invite_id", inv.id}, {"request_id", request_id}, {"title", it->second.first.title},
                                {"about", it->second.first.about}},
                     now, &ignored);
  }
  return inv.return_path.empty() ? std::string("/") : inv.return_path;
}

void Host::remind_invites(int64_t now_ms) {
  std::set<std::string> tenants;
  for (const auto& [d, e] : config_.get()->domains) tenants.insert(e.tenant);
  for (const auto& tenant : tenants) {
    for (const auto& inv : identity_->due_reminders(tenant, now_ms)) {
      // Marked first: a reminder that can't go now never goes twice later.
      identity_->mark_reminded(tenant, inv.id, now_ms);
      std::string why;
      if (!send_invite_mail(tenant, inv, true, &why) && why != "blocked") {
        std::cerr << "goober-host: invite " << inv.id << " in " << tenant << ": reminder not sent: " << why << "\n";
      }
    }
  }
}

// /identity/invite/$id/$mac and /identity/invite/$id/$mac/decline.
void Host::handle_invite(const Request& req, const Finish& finish, const std::string& path, const std::string& domain,
                         const std::string& tenant) {
  std::string request_id = req.header("goober-request-id");
  std::string rest = path.substr(std::string("/identity/invite/").size());
  bool decline = rest.ends_with("/decline");
  if (decline) rest = rest.substr(0, rest.size() - 8);
  auto slash = rest.find('/');
  int64_t id = 0;
  try {
    id = slash == std::string::npos ? 0 : std::stoll(rest.substr(0, slash));
  } catch (...) {
  }
  auto inv = id > 0 ? identity_->invite(tenant, id) : std::nullopt;
  std::string mac = slash == std::string::npos ? "" : rest.substr(slash + 1);
  if (!inv || !same(mac, invite_mac(tenant, inv->id, inv->email))) {
    return finish(error_page(tenant, 404, request_id), R"({"invite":"bad link"})");
  }
  auto cfg = config_.get();
  const auto& tc = cfg->tenant(tenant);
  std::string site = site_of(tenant);
  auto page = [&](int status, const std::string& title, const std::string& body) {
    return identity_page(tenant, status, title, body);
  };
  auto when = [](int64_t ms) { return base::format_utc_ms(ms).substr(0, 10); };
  std::string self = "/identity/invite/" + std::to_string(inv->id) + "/" + mac;
  int64_t now = base::now_unix_ms();
  bool is_post = req.method == "post";
  if (!is_post && req.method != "get" && req.method != "head") return finish(error_page(tenant, 405, request_id), "");

  if (decline) {
    if (inv->state == "declined") {
      return finish(page(200, "Declined", "<h1>You declined</h1><p>On " + when(inv->answered_at) + ". " + html_escape(site) +
                                              " won't invite or email you again.</p>"),
                    R"({"invite":"already_declined"})");
    }
    if (is_post) {
      // The one-click POST (RFC 8058) comes from the mail provider: the link's MAC is the whole
      // authority, and declining only ever reduces mail.
      auto form = router::parse_query(req.body);
      std::string how = form.count("List-Unsubscribe") ? "one_click" : "page";
      std::string why;
      bool was_answerable = inv->state == "open" || inv->state == "lapsed";
      if (was_answerable) identity_->decline_invite(tenant, inv->id, how, now, &why);
      consent_->add_block(tenant, address_hash(inv->email), "invite_declined",
                          bj::object{{"invite_id", inv->id}, {"how", how}, {"request_id", request_id}}, now);
      return finish(page(200, "Declined",
                         "<h1>Done</h1><p>" + html_escape(site) + " won't invite or email you again.</p><p class=\"muted\">"
                         "If you change your mind, sign in on the site and allow mail on your account page.</p>"),
                    json::dump(bj::object{{"invite", "declined"}, {"how", how}}));
    }
    return finish(page(200, "Decline?",
                       "<h1>Don't invite me to " + html_escape(site) + " again?</h1><p>" + html_escape(site) +
                           " won't invite or email you after this. Sign-in codes you ask for yourself still come.</p>"
                           "<form method=\"post\"><button>Yes, stop</button></form>"),
                  R"({"invite":"decline_asked"})");
  }

  // Accepting.
  if (inv->state != "open" || inv->expires_at <= now) {
    std::string what = inv->state == "accepted"   ? "You accepted this on " + when(inv->answered_at) + "."
                       : inv->state == "declined" ? "You declined this on " + when(inv->answered_at) + "."
                       : inv->state == "revoked"  ? "This invite was withdrawn."
                                                  : "This invite lapsed.";
    return finish(page(410, "Invite", "<h1>This invite is closed</h1><p>" + html_escape(what) +
                                          "</p><p><a href=\"/\">Go to " + html_escape(site) + "</a></p>"),
                  json::dump(bj::object{{"invite", "closed"}, {"state", inv->state}}));
  }
  auto who = browser_session(req, tenant);
  if (who && who->host) who.reset();
  if (is_post) {
    if (!same_origin(req, origin_of(req))) return finish(error_page(tenant, 403, request_id), R"({"invite":"cross-origin"})");
    if (who && who->user.email == inv->email) {
      std::string why;
      auto next = accept_invite_for(tenant, *inv, who->user, "session", request_id, &why);
      if (!next) return finish(page(409, "Invite", "<h1>Not accepted</h1><p>" + html_escape(why) + "</p>"), R"({"invite":"refused"})");
      Response res;
      res.status = 303;
      res.headers.emplace_back("location", safe_return(*next));
      res.headers.emplace_back("cache-control", "no-store");
      return finish(std::move(res), json::dump(bj::object{{"invite", "accepted"}, {"how", "session"}}));
    }
    if (who) {
      return finish(page(409, "Invite", "<h1>This invite is for someone else</h1><p>You're signed in as " +
                                            html_escape(who->user.email) + ". Sign out, then open the invite again.</p>"
                                            "<form method=\"post\" action=\"/identity/sign-out\"><input type=\"hidden\" "
                                            "name=\"return\" value=\"" + html_escape(self) + "\"><button>Sign out</button></form>"),
                    R"({"invite":"other_user"})");
    }
    // The code proves the address; the link alone never signs anyone in (§ 2.4).
    identity_->start_email(tenant, inv->email, domain, true, now);
    Response res;
    res.status = 303;
    res.headers.emplace_back("location", "/identity/sign-in/code?email=" + base::percent_encode(inv->email) +
                                             "&invite=" + std::to_string(inv->id) + "." + mac);
    res.headers.emplace_back("cache-control", "no-store");
    return finish(std::move(res), R"({"invite":"code_sent"})");
  }
  std::string inviter = inv->inviter_kind == "site"       ? site
                        : inv->inviter_kind == "operator" ? (inv->inviter_name.empty() ? "The team at " + site : inv->inviter_name)
                        : inv->inviter_name.empty()       ? inv->inviter_email
                                                          : inv->inviter_name + " (" + inv->inviter_email + ")";
  std::string body = "<h1>Join " + html_escape(site) + "</h1><p>" + html_escape(inviter) + " invited <strong>" +
                     html_escape(inv->email) + "</strong>.</p>";
  if (!inv->note.empty()) body += "<blockquote>" + html_escape(inv->note) + "</blockquote>";
  if (!inv->roles.empty()) body += "<p>You'll be: " + html_escape(role_words(inv->roles)) + ".</p>";
  auto lists = tenant_lists(tenant);
  std::string list_items;
  for (const auto& l : inv->lists) {
    auto it = lists.find(l);
    if (it != lists.end()) {
      list_items += "<li><strong>" + html_escape(it->second.first.title) + "</strong> <span class=\"muted\">" +
                    html_escape(it->second.first.about) + "</span></li>";
    }
  }
  if (!list_items.empty()) body += "<p>You'll get:</p><ul>" + list_items + "</ul><p class=\"muted\">Every mail has a link to leave.</p>";
  if (!tc.terms.empty()) {
    body += "<p class=\"muted\">By continuing, you accept " + html_escape(site) + "'s <a href=\"" + html_escape(tc.terms) +
            "\">terms</a>" + (tc.privacy.empty() ? "" : " and <a href=\"" + html_escape(tc.privacy) + "\">privacy policy</a>") +
            ".</p>";
  }
  if (who && who->user.email != inv->email) {
    body += "<p class=\"error\">You're signed in as " + html_escape(who->user.email) + ". This invite is for " +
            html_escape(inv->email) + ".</p>";
  }
  body += "<form method=\"post\"><button>Continue</button></form><p class=\"muted\"><a href=\"" + html_escape(self) +
          "/decline\">No thanks, don't invite me again</a></p>";
  return finish(page(200, "Join " + site, body), R"({"invite":"page"})");
}

std::string Host::invites_call(CallContext& ctx, const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                               const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  if (!request.is_object()) return fail("invites." + m.name + " takes a JSON object");
  const auto& r = request.as_object();
  auto cfg = config_.get();
  const auto& tc = cfg->tenant(ctx.tenant);
  if (m.name == "status") {
    std::string email = base::to_lower(field(r, "email"));
    if (email.find('@') == std::string::npos) return fail("status takes {email}");
    auto list = identity_->invites(ctx.tenant, "", email, "", 1);
    bj::object out{{"email", email}, {"state", list.empty() ? "none" : list[0].state}};
    if (!list.empty()) out["invite_id"] = std::to_string(list[0].id);
    std::string why;
    if (consent_->block_of(ctx.tenant, email)) why = "blocked";
    else if (identity_->proved(ctx.tenant, email)) why = "member";
    else if (!list.empty() && list[0].state == "open") why = "open";
    else if (!list.empty() && (list[0].state == "lapsed" || list[0].state == "revoked") &&
             list[0].answered_at > base::now_unix_ms() - identity::kInviteCoolMs) why = "cooling";
    out["can_invite"] = why.empty();
    if (!why.empty()) out["why"] = why;
    return json::dump(out);
  }
  if (m.name == "revoke") {
    std::string ids = field(r, "invite_id");
    int64_t id = 0;
    try {
      id = ids.empty() && r.contains("invite_id") && r.at("invite_id").is_int64() ? r.at("invite_id").as_int64() : std::stoll(ids);
    } catch (...) {
    }
    auto inv = id > 0 ? identity_->invite(ctx.tenant, id) : std::nullopt;
    if (!inv || inv->box != ctx.manifest->box) return fail("no invite " + ids + " from this box");
    std::string why;
    if (!identity_->revoke_invite(ctx.tenant, id, "box:" + ctx.manifest->box, base::now_unix_ms(), &why)) return fail(why);
    return json::dump(bj::object{{"revoked", true}});
  }
  // send: checked now, so the box learns at once; made and mailed after the request commits.
  for (const auto& kv : r) {
    static const std::set<std::string, std::less<>> kFields = {"email", "roles", "lists", "note", "inviter_name", "return"};
    if (!kFields.count(kv.key())) return fail("send takes {email, roles?, lists?, note?, inviter_name?, return?}; \"" + std::string(kv.key()) + "\" isn't one");
  }
  std::string email = base::to_lower(field(r, "email"));
  if (!plain_email(email)) return fail("email is one address");
  bool ok = true;
  auto roles = strings(r, "roles", &ok);
  auto lists = strings(r, "lists", &ok);
  if (!ok) return fail("roles and lists are lists of names");
  for (const auto& role : roles) {
    if (std::find(svc.invite_roles.begin(), svc.invite_roles.end(), role) == svc.invite_roles.end()) {
      return fail("role " + role + " isn't one this service's invites declare");
    }
    if (std::find(tc.invites.roles.begin(), tc.invites.roles.end(), role) == tc.invites.roles.end()) {
      return fail("role " + role + " isn't in the host config's tenants." + ctx.tenant + ".invites.roles");
    }
  }
  for (const auto& l : lists) {
    if (std::find(svc.invite_lists.begin(), svc.invite_lists.end(), l) == svc.invite_lists.end()) {
      return fail("list " + l + " isn't one this service's invites declare");
    }
  }
  std::string note = field(r, "note"), inviter_name = field(r, "inviter_name"), ret = field(r, "return");
  if (note.size() > 500) return fail("note is at most 500 characters");
  if (inviter_name.size() > 64 || inviter_name.find_first_of("<>\r\n\"") != std::string::npos) {
    return fail("inviter_name is at most 64 characters, without <>, quotes, or newlines");
  }
  if (!ret.empty() && (ret[0] != '/' || ret.starts_with("//") || ret.size() > 200)) return fail("return is a path on the site, like /welcome");
  // Who invited: the person signed in on this request, or else the site; never a name the box makes up.
  identity::Invite inv;
  inv.email = email;
  inv.roles = roles;
  inv.lists = lists;
  inv.note = note;
  inv.return_path = ret.empty() ? "/" : ret;
  inv.box = ctx.manifest->box;
  if (ctx.who && !ctx.who->host) {
    inv.inviter_kind = "person";
    inv.inviter_email = ctx.who->user.email;
    inv.inviter_name = inviter_name;
  } else {
    if (!inviter_name.empty()) return fail("inviter_name needs a signed-in person: with nobody signed in, the site invites");
    inv.inviter_kind = "site";
  }
  if (tc.postal_address.empty()) {
    return fail("invites need the tenant's postal address: set tenants." + ctx.tenant + ".postal-address in the host config");
  }
  if (consent_->block_of(ctx.tenant, email)) return fail("blocked: " + email + " asked for no invites from this site");
  if (identity_->proved(ctx.tenant, email)) return fail("member: " + email + " already has an account here");
  if (identity_->open_invite_for(ctx.tenant, email)) return fail("open: an invite to " + email + " is still open");
  OutboxElement e;
  e.service = svc.name;
  e.method = "send";
  e.verb = "INVITE";
  e.url = "invite:" + email;
  bj::array ra, la;
  for (const auto& x : roles) ra.push_back(bj::value(x));
  for (const auto& x : lists) la.push_back(bj::value(x));
  e.body = json::dump(bj::object{{"email", email}, {"roles", std::move(ra)}, {"lists", std::move(la)}, {"note", note},
                                 {"inviter_name", inv.inviter_name}, {"inviter_email", inv.inviter_email},
                                 {"inviter_kind", inv.inviter_kind}, {"box", inv.box}, {"return", inv.return_path}});
  e.idempotency_key = base::sha256_hex("goober-outbox\n" + ctx.request_id + "\n" + std::to_string(ctx.pending.size())).substr(0, 32);
  e.attempts = m.attempts;
  e.backoff_ms = m.backoff_ms;
  e.within_ms = m.within_ms;
  std::string key = e.idempotency_key;
  ctx.pending.push_back(std::move(e));
  return json::dump(bj::object{{"accepted", true}, {"key", key}});
}

HttpOut Host::deliver_outbox_invite(const std::string& tenant, const std::string& body, const std::string& key,
                                    int64_t now_ms) {
  HttpOut out;
  std::string err;
  auto v = json::parse(body, &err);
  if (!v || !v->is_object()) {
    out.status = 400;
    out.error = "the invite element isn't readable";
    return out;
  }
  const auto& o = v->as_object();
  identity::Invite inv;
  inv.email = field(o, "email");
  bool ok = true;
  inv.roles = strings(o, "roles", &ok);
  inv.lists = strings(o, "lists", &ok);
  inv.note = field(o, "note");
  inv.inviter_name = field(o, "inviter_name");
  inv.inviter_email = field(o, "inviter_email");
  inv.inviter_kind = field(o, "inviter_kind");
  inv.box = field(o, "box");
  inv.return_path = field(o, "return");
  // The host config may have dropped a role since the call: the invite goes without it.
  const auto& allowed = config_.get()->tenant(tenant).invites.roles;
  std::erase_if(inv.roles, [&](const std::string& r) { return std::find(allowed.begin(), allowed.end(), r) == allowed.end(); });
  std::string code, why;
  auto made = make_invite(tenant, inv, &code, &why);
  if (made) {
    out.status = 200;
    out.body = std::to_string(made->id);
    return out;
  }
  // A retry after the record was made (the mail queue took it): already done.
  if (code == "open") {
    if (auto open = identity_->open_invite_for(tenant, inv.email); open && open->box == inv.box && open->created_at > now_ms - kDay) {
      out.status = 200;
      out.body = std::to_string(open->id);
      return out;
    }
  }
  out.status = code == "limit" ? 429 : 403;
  out.error = code + ": " + why;
  (void)key;
  return out;
}

void Host::register_invite_tools() {
  using mcp::object_schema;
  using mcp::string_property;
  auto known = [this](const std::string& tenant) {
    for (const auto& [d, e] : config_.get()->domains) {
      if (e.tenant == tenant) return true;
    }
    return false;
  };
  auto array_property = [](const char* what) {
    return bj::value(bj::object{{"type", "array"}, {"items", bj::object{{"type", "string"}}}, {"description", what}});
  };
  mcp_->add_tool(
      {"invite_send",
       "Invites one person to a tenant's site, by mail from the host (029 § 2): the invite names who asked, what "
       "accepting grants (roles), the lists it joins, and the site's terms; it has an accept link (a sign-in code "
       "proves the address) and a decline link that stops all of the site's mail to them for good. One reminder "
       "after 7 days; it lapses at 30, and the address can't be invited again for 90. Refused (with code) for a "
       "blocked address, an account, an open invite, a cooling address, or no postal address in the host config.",
       object_schema({{"tenant", string_property("A tenant name from the host config.")},
                      {"email", string_property("The one address invited.")},
                      {"roles", array_property("Tenant roles granted on acceptance, like member.")},
                      {"lists", array_property("Lists joined on acceptance (mail_lists names them).")},
                      {"note", string_property("Up to 500 characters, shown as a quote from the inviter.")},
                      {"inviter_name", string_property("Who invites, as shown; default \"The team at {site}\".")},
                      {"return", string_property("Where accepting lands, a path like /welcome.")}},
                     {"tenant", "email"}),
       [this, known](const bj::object& args) {
         std::string tenant = field(args, "tenant");
         if (!known(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
         identity::Invite inv;
         inv.email = field(args, "email");
         bool ok = true;
         inv.roles = strings(args, "roles", &ok);
         inv.lists = strings(args, "lists", &ok);
         if (!ok) return ToolResult::fail("roles and lists are lists of names");
         inv.note = field(args, "note");
         if (inv.note.size() > 500) return ToolResult::fail("note is at most 500 characters");
         inv.inviter_name = field(args, "inviter_name");
         if (inv.inviter_name.size() > 64) return ToolResult::fail("inviter_name is at most 64 characters");
         inv.inviter_kind = "operator";
         inv.return_path = field(args, "return");
         if (inv.return_path.empty()) inv.return_path = "/";
         if (inv.return_path[0] != '/' || inv.return_path.starts_with("//")) return ToolResult::fail("return is a path, like /welcome");
         std::string code, why;
         auto made = make_invite(tenant, inv, &code, &why);
         if (!made) return ToolResult::fail(code + ": " + why);
         auto out = made->to_json();
         out["accept_url"] = invite_url(tenant, *made, false);
         return ToolResult::ok(std::move(out));
       }});
  mcp_->add_tool({"invite_list",
                  "A tenant's invites, newest first: each with its state (open, accepted, declined, lapsed, revoked), "
                  "who invited, what it grants, and its dates. Filter by state, email, or box.",
                  object_schema({{"tenant", string_property("A tenant name from the host config.")},
                                 {"state", string_property("open, accepted, declined, lapsed, or revoked.")},
                                 {"email", string_property("One address.")},
                                 {"box", string_property("The box that sent them.")},
                                 {"limit", mcp::integer_property("1 to 500; default 100.")}},
                                {"tenant"}),
                  [this, known](const bj::object& args) {
                    std::string tenant = field(args, "tenant");
                    if (!known(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
                    int64_t limit = 100;
                    if (auto* l = args.if_contains("limit"); l && l->is_int64()) limit = std::clamp<int64_t>(l->as_int64(), 1, 500);
                    bj::array out;
                    for (const auto& i : identity_->invites(tenant, field(args, "state"), field(args, "email"), field(args, "box"), int(limit))) {
                      out.push_back(i.to_json());
                    }
                    return ToolResult::ok(bj::object{{"tenant", tenant}, {"invites", std::move(out)}});
                  }});
  mcp_->add_tool({"invite_get", "One invite, with its accept link (to resend by hand, say).",
                  object_schema({{"tenant", string_property("A tenant name from the host config.")},
                                 {"invite_id", string_property("The invite's id.")}},
                                {"tenant", "invite_id"}),
                  [this, known](const bj::object& args) {
                    std::string tenant = field(args, "tenant");
                    if (!known(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
                    int64_t id = 0;
                    try {
                      id = std::stoll(field(args, "invite_id"));
                    } catch (...) {
                    }
                    auto inv = identity_->invite(tenant, id);
                    if (!inv) return ToolResult::fail("no such invite");
                    auto out = inv->to_json();
                    if (inv->state == "open") out["accept_url"] = invite_url(tenant, *inv, false);
                    return ToolResult::ok(std::move(out));
                  }});
  mcp_->add_tool({"invite_revoke", "Withdraws an open invite: its links then say so, and the address cools for 90 days.",
                  object_schema({{"tenant", string_property("A tenant name from the host config.")},
                                 {"invite_id", string_property("The invite's id.")}},
                                {"tenant", "invite_id"}),
                  [this, known](const bj::object& args) {
                    std::string tenant = field(args, "tenant");
                    if (!known(tenant)) return ToolResult::fail("no domain in the host config serves tenant " + tenant);
                    int64_t id = 0;
                    try {
                      id = std::stoll(field(args, "invite_id"));
                    } catch (...) {
                    }
                    std::string why;
                    if (!identity_->revoke_invite(tenant, id, "operator:" + current_operator(), base::now_unix_ms(), &why)) {
                      return ToolResult::fail(why);
                    }
                    return ToolResult::ok(bj::object{{"invite_id", std::to_string(id)}, {"state", "revoked"}});
                  }});
}

}  // namespace goober::host
