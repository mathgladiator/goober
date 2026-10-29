// The host-owned identity endpoints under /identity/ (013.Identity.md § The implicit
// API; design/machine/006.LocalIdentity.md): a sign-in page with emailed codes, a
// session cookie, an account page, sign-out, and /identity/me.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "http_util.h"
#include "goober/json/json.h"
#include "goober/router/path.h"

namespace goober::host {
namespace bj = boost::json;
using base::html_escape;
using server::http::Request;
using server::http::Response;

namespace {

std::string read_cookie(const Request& req, std::string_view name) {
  std::string header = req.header("cookie");
  size_t i = 0;
  while (i < header.size()) {
    size_t end = header.find(';', i);
    if (end == std::string::npos) end = header.size();
    std::string_view pair = std::string_view(header).substr(i, end - i);
    while (!pair.empty() && pair.front() == ' ') pair.remove_prefix(1);
    if (pair.starts_with(name) && pair.size() > name.size() && pair[name.size()] == '=') {
      return std::string(pair.substr(name.size() + 1));
    }
    i = end + 1;
  }
  return "";
}

constexpr const char* kStyle = R"css(
:root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fafaf7;--muted:#6b6b6b;--card:#fff;--line:#ddd;--accent:#2457d6}
@media (prefers-color-scheme:dark){:root{--fg:#ececec;--bg:#141414;--muted:#9a9a9a;--card:#1e1e1e;--line:#333;--accent:#7aa2ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,sans-serif}
main{max-width:26rem;margin:10vh auto;padding:0 16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:24px}
h1{font-size:1.4rem;margin:0 0 4px}p{margin:8px 0}.muted{color:var(--muted);font-size:.9rem}
label{display:block;margin:16px 0 4px;font-weight:600}
input[type=email],input[type=text]{width:100%;padding:10px 12px;font:inherit;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg)}
.check{font-weight:400;display:flex;gap:8px;align-items:center}
button{margin-top:16px;width:100%;padding:10px;font:inherit;font-weight:600;border:0;border-radius:8px;background:var(--accent);color:#fff;cursor:pointer}
a.button{display:block;margin-top:16px;padding:10px;text-align:center;font-weight:600;border-radius:8px;background:var(--accent);color:#fff;text-decoration:none}
button.secondary{margin-top:8px;background:transparent;color:var(--fg);border:1px solid var(--line)}
button.small{width:auto;margin:0;padding:4px 10px;font-size:.85rem}
li form{display:inline}
.error{color:#c0392b;font-weight:600}.local{border-left:3px solid var(--accent);padding-left:10px}
code{font-size:.9em}ul{padding-left:20px}a{color:var(--accent)}
)css";

}  // namespace

std::string Host::session_cookie_name() const {
  return config_.get()->identity.cookie_secure ? "__Host-goober-session" : "goober-session";
}

std::optional<SignedIn> Host::signed_in(const Request& req, const std::string& tenant) {
  // A bearer token (OAuth, 009) speaks for itself: a bad one is nobody, never the cookie.
  std::string auth = req.header("authorization");
  if (auth.size() > 7 && base::to_lower(auth.substr(0, 7)) == "bearer ") {
    auto b = identity_->bearer(tenant, auth.substr(7), base::now_unix_ms());
    if (!b) return std::nullopt;
    auto roles = b->user.roles;
    return SignedIn{std::move(b->user), false, std::move(roles), b->agent, b->client_id};
  }
  return browser_session(req, tenant);
}

std::optional<SignedIn> Host::browser_session(const Request& req, const std::string& tenant) {
  std::string value = read_cookie(req, session_cookie_name());
  if (value.size() < 3 || value[1] != '.') return std::nullopt;
  std::string token = value.substr(2);
  int64_t now = base::now_unix_ms();
  if (value[0] == 'h') {
    auto user = identity_->session(identity::kHostRealm, token, now);
    if (!user) return std::nullopt;
    auto cfg = config_.get();
    auto it = cfg->identity.operators.find(user->email);
    // An address removed from identity.operators holds no host roles, so it is signed out.
    if (it == cfg->identity.operators.end() || it->second.empty()) return std::nullopt;
    return SignedIn{std::move(*user), true, it->second, false, ""};
  }
  if (value[0] == 't') {
    auto user = identity_->session(tenant, token, now);
    if (!user) return std::nullopt;
    auto roles = user->roles;
    return SignedIn{std::move(*user), false, std::move(roles), false, ""};
  }
  return std::nullopt;
}

Response Host::identity_page(const std::string& tenant, int status, const std::string& title,
                             const std::string& body, const std::string& form_action_extra, bool tenant_css) const {
  Response res;
  res.status = status;
  // A tenant may restyle sign-in (Q47), but not the consent screens, where its stylesheet
  // could hide "unverified" or swap the buttons (013 § Sign-in screen requirements).
  std::string css_link = tenant_css && content_->get(tenant, "sign-in.css")
                             ? "<link rel=\"stylesheet\" href=\"/sign-in.css\">"
                             : "";
  res.body = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>" +
             html_escape(title) + "</title><style>" + kStyle + "</style>" + css_link +
             "</head><body><main><div class=\"card\">" + body + "</div></main></body></html>\n";
  res.headers.emplace_back("content-type", "text/html; charset=utf-8");
  res.headers.emplace_back("cache-control", "no-store");
  res.headers.emplace_back("x-frame-options", "DENY");
  // Not no-referrer: under it browsers post forms with "Origin: null", which same_origin refuses.
  res.headers.emplace_back("referrer-policy", "same-origin");
  res.headers.emplace_back("x-content-type-options", "nosniff");
  res.headers.emplace_back("content-security-policy",
                           std::string(tenant_css ? "default-src 'none'; style-src 'self' 'unsafe-inline'"
                                                  : "default-src 'none'; style-src 'unsafe-inline'") +
                               "; form-action 'self'" +
                               (form_action_extra.empty() ? "" : " " + form_action_extra) +
                               "; frame-ancestors 'none'; base-uri 'none'");
  return res;
}

void Host::handle_identity(const Request& req, const Finish& finish, const std::string& path,
                           const std::string& domain, const std::string& tenant) {
  auto cfg = config_.get();
  std::string request_id = req.header("goober-request-id");
  auto query = router::parse_query(req.query);
  auto form = router::parse_query(req.body);

  auto page = [&](int status, const std::string& title, const std::string& body) {
    return identity_page(tenant, status, title, body);
  };
  auto redirect = [&](const std::string& to) {
    Response res;
    res.status = 303;
    res.headers.emplace_back("location", to);
    res.headers.emplace_back("cache-control", "no-store");
    return res;
  };
  auto cookie = [&](const std::string& value, int64_t max_age_s) {
    std::string c = session_cookie_name() + "=" + value + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
                    std::to_string(max_age_s);
    if (cfg->identity.cookie_secure) c += "; Secure";
    return c;
  };
  auto local_note = [&]() -> std::string {
    if (!cfg->local) return "";
    return "<p class=\"muted local\">Local mode: no mail leaves this machine. The code is printed in the terminal "
           "running goober-host, and saved under <code>storage/mail/outbox/</code>.</p>";
  };
  // Host operators sign in at /identity/sign-in/as-host, which nothing links to, so a
  // tenant's customers never see a way in to the host.
  auto sign_in_form = [&](const std::string& ret, bool as_host, const std::string& error) {
    return std::string(as_host ? "<h1>Host sign-in</h1>" : "<h1>Sign in</h1>") + "<p class=\"muted\">" +
           (as_host ? "as a host operator, acting on <strong>" : "to <strong>") + html_escape(domain) +
           "</strong></p>" + (error.empty() ? "" : "<p class=\"error\">" + html_escape(error) + "</p>") +
           "<form method=\"post\" action=\"/identity/email/start\"><label for=\"email\">Email</label>"
           "<input id=\"email\" type=\"email\" name=\"email\" required autofocus autocomplete=\"email\">"
           "<input type=\"hidden\" name=\"return\" value=\"" + html_escape(ret) + "\">" +
           (as_host ? "<input type=\"hidden\" name=\"as\" value=\"host\">" : "") +
           "<button>Email me a code</button></form>"
           "<p class=\"muted\">We'll email you a six-digit code to type here. Nothing to remember.</p>" +
           local_note();
  };
  auto code_form = [&](const std::string& email, const std::string& ret, bool as_host, const std::string& error) {
    return "<h1>Check your email</h1><p>If <strong>" + html_escape(email) + "</strong> can sign in to <strong>" +
           html_escape(domain) + "</strong>" + (as_host ? " as a host operator" : "") +
           ", a code is on its way. It expires in 10 minutes.</p>" +
           (error.empty() ? "" : "<p class=\"error\">" + html_escape(error) + "</p>") +
           "<form method=\"post\" action=\"/identity/email/verify\"><label for=\"code\">Code</label>"
           "<input id=\"code\" type=\"text\" name=\"code\" required autofocus inputmode=\"numeric\" "
           "autocomplete=\"one-time-code\" pattern=\"[0-9]{6}\" maxlength=\"6\">"
           "<input type=\"hidden\" name=\"email\" value=\"" + html_escape(email) + "\">"
           "<input type=\"hidden\" name=\"return\" value=\"" + html_escape(ret) + "\">" +
           (as_host ? "<input type=\"hidden\" name=\"as\" value=\"host\">" : "") +
           "<button>Sign in</button></form><p class=\"muted\">Only continue if you started this sign-in yourself. "
           "<a href=\"/identity/sign-in" + (as_host ? "/as-host" : "") + "?return=" + base::percent_encode(ret) +
           "\">Use a different address</a>.</p>" +
           local_note();
  };
  auto get = [](const std::map<std::string, std::string>& m, const char* k) {
    auto it = m.find(k);
    return it == m.end() ? std::string() : it->second;
  };

  bool is_post = req.method == "post";
  bool is_get = req.method == "get" || req.method == "head";
  if (is_post && !same_origin(req, origin_of(req))) {
    return finish(error_page(tenant, 403, request_id), R"({"identity":"cross-origin post"})");
  }
  int64_t now = base::now_unix_ms();

  if ((path == "/identity/sign-in" || path == "/identity/sign-in/as-host") && is_get) {
    bool as_host = path == "/identity/sign-in/as-host";
    return finish(page(200, as_host ? "Host sign-in" : "Sign in",
                       sign_in_form(safe_return(get(query, "return")), as_host, "")),
                  as_host ? R"({"identity":"host_sign_in_page"})" : R"({"identity":"sign_in_page"})");
  }
  if (path == "/identity/email/start" && is_post) {
    std::string email = base::to_lower(get(form, "email"));
    bool as_host = get(form, "as") == "host";
    std::string ret = safe_return(get(form, "return"));
    if (as_host) {
      bool is_operator = cfg->identity.operators.count(email) > 0;
      identity_->start_email(identity::kHostRealm, email, domain, is_operator, now);
    } else {
      identity_->start_email(tenant, email, domain, true, now);
    }
    return finish(page(200, "Check your email", code_form(email, ret, as_host, "")), R"({"identity":"email_start"})");
  }
  if (path == "/identity/email/verify" && is_post) {
    std::string email = base::to_lower(get(form, "email"));
    bool as_host = get(form, "as") == "host";
    std::string ret = safe_return(get(form, "return"));
    std::string why;
    int64_t session_ms = cfg->identity.session_days * 24 * 3600 * 1000;
    std::optional<identity::SignIn> in;
    if (as_host) {
      bool is_operator = cfg->identity.operators.count(email) > 0;
      in = identity_->verify_email(identity::kHostRealm, email, get(form, "code"), is_operator, now, session_ms, &why);
    } else {
      in = identity_->verify_email(tenant, email, get(form, "code"), true, now, session_ms, &why);
      // The first owner named in the config gets "owner" whenever the realm has none.
      auto entry = cfg->resolve_host(req.header("host"));
      if (in && entry && entry->second.owner == in->user.email && identity_->holders(tenant, "owner") == 0) {
        std::string err;
        identity_->grant(tenant, in->user.email, "owner", "config:first-owner", now, &err);
      }
    }
    if (!in) {
      return finish(page(400, "Check your email", code_form(email, ret, as_host, why)), R"({"identity":"verify_failed"})");
    }
    auto res = redirect(ret);
    res.headers.emplace_back("set-cookie",
                             cookie(std::string(as_host ? "h." : "t.") + in->token, session_ms / 1000));
    return finish(std::move(res), json::dump(bj::object{{"identity", "sign_in"},
                                                       {"realm", in->user.realm},
                                                       {"user_id", std::to_string(in->user.user_id)}}));
  }
  if (path == "/identity/sign-out" && is_post) {
    std::string value = read_cookie(req, session_cookie_name());
    if (value.size() > 2) {
      identity_->end_session(value[0] == 'h' ? std::string(identity::kHostRealm) : tenant, value.substr(2), now);
    }
    auto res = redirect(safe_return(get(form, "return")));
    res.headers.emplace_back("set-cookie", cookie("", 0));
    return finish(std::move(res), R"({"identity":"sign_out"})");
  }
  if (path == "/identity/sessions/end" && is_post) {
    auto who = browser_session(req, tenant);
    if (who && !who->host) {
      try {
        identity_->end_session_by_id(tenant, who->user.user_id, std::stoll(get(form, "id")), now);
      } catch (...) {
      }
    }
    return finish(redirect("/identity/account"), R"({"identity":"session_ended"})");
  }
  if (path == "/identity/grants/revoke" && is_post) {
    auto who = browser_session(req, tenant);
    if (who && !who->host) identity_->revoke_grant(tenant, who->user.user_id, get(form, "grant"), now);
    return finish(redirect("/identity/account"), R"({"identity":"grant_revoked"})");
  }
  if ((path == "/identity/me" || path == "/identity/account") && is_get) {
    auto who = path == "/identity/me" ? signed_in(req, tenant) : browser_session(req, tenant);
    if (path == "/identity/me") {
      Response res;
      res.headers.emplace_back("content-type", "application/json");
      res.headers.emplace_back("cache-control", "no-store");
      if (!who) {
        res.status = 401;
        res.body = R"({"signed_in":false})";
      } else {
        bj::array roles;
        for (const auto& r : who->roles) roles.push_back(bj::value(r));
        res.body = json::dump(bj::object{{"signed_in", true},
                                         {"user_id", std::to_string(who->user.user_id)},
                                         {"email", who->user.email},
                                         {"realm", who->user.realm},
                                         {"host", who->host},
                                         {"roles", std::move(roles)},
                                         {"domain", domain},
                                         {"agent", who->agent},
                                         {"client_id", who->client_id}});
      }
      return finish(std::move(res), "");
    }
    if (!who) return finish(redirect("/identity/sign-in?return=%2Fidentity%2Faccount"), "");
    std::string roles;
    for (const auto& r : who->roles) roles += "<li><code>" + html_escape(r) + "</code></li>";
    if (roles.empty()) roles = "<li class=\"muted\">none yet</li>";
    // Apps and agents signed in as this person (013 § Sign-in screen requirements: every
    // session is visible, with a way to revoke it).
    std::string apps;
    if (!who->host) {
      for (const auto& g : identity_->grants(tenant, who->user.user_id, now)) {
        const auto& o = g.as_object();
        bool person = o.at("acts_as") == "person";
        apps += "<li><strong>" + html_escape(std::string(o.at("name").as_string())) + "</strong> <span class=\"muted\">" +
                (person ? "acts as you" : "an agent acting for you") +
                (o.at("source") == "dynamic" ? ", unverified" : "") +
                "</span> <form method=\"post\" action=\"/identity/grants/revoke\"><input type=\"hidden\" "
                "name=\"grant\" value=\"" + html_escape(std::string(o.at("grant").as_string())) +
                "\"><button class=\"small secondary\">Remove</button></form></li>";
      }
      apps = "<p><strong>Apps and agents</strong></p><ul>" +
             (apps.empty() ? std::string("<li class=\"muted\">none signed in</li>") : apps) + "</ul>";
      // Browser sessions too, so a stolen cookie can be ended from here.
      std::string sessions;
      for (const auto& sv : identity_->sessions(tenant, who->user.user_id, now)) {
        const auto& o = sv.as_object();
        sessions += "<li>A browser, signed in until " + base::format_utc_ms(o.at("expires_at").as_int64()).substr(0, 10) +
                    " <form method=\"post\" action=\"/identity/sessions/end\"><input type=\"hidden\" name=\"id\" "
                    "value=\"" + std::to_string(o.at("id").as_int64()) +
                    "\"><button class=\"small secondary\">End</button></form></li>";
      }
      apps += "<p><strong>Browser sessions</strong> <span class=\"muted\">(including this one)</span></p><ul>" +
              sessions + "</ul>";
      // What agents did for this person, newest first (050 § The audit).
      std::string history;
      int shown = 0;
      auto lines = base::tail_lines(cfg->storage / "logs" / "agents.log", 2000);
      for (auto it = lines.rbegin(); it != lines.rend() && shown < 20; ++it) {
        std::string e;
        auto v = json::parse(*it, &e);
        if (!v || !v->is_object()) continue;
        const auto& o = v->as_object();
        auto text = [&](const char* k) {
          auto* f = o.if_contains(k);
          return f && f->is_string() ? std::string(f->as_string()) : std::string();
        };
        auto* agent = o.if_contains("agent");
        if (text("tenant") != tenant || text("user_id") != std::to_string(who->user.user_id) || !agent ||
            !agent->is_bool() || !agent->as_bool() || text("tool").empty()) {
          continue;
        }
        std::string client = text("client_id");
        if (auto c = identity_->client(tenant, client)) client = c->name;
        int64_t status = o.contains("status") && o.at("status").is_int64() ? o.at("status").as_int64() : 0;
        history += "<li><code>" + html_escape(text("tool")) + "</code> by " + html_escape(client) +
                   " <span class=\"muted\">" + html_escape(text("at").substr(0, 16)) +
                   (status >= 400 ? " · refused" : "") + "</span></li>";
        ++shown;
      }
      if (!history.empty()) apps += "<p><strong>What your agents did</strong></p><ul>" + history + "</ul>";
    }
    return finish(page(200, "Your account",
                       "<h1>" + html_escape(who->user.email) + "</h1><p class=\"muted\">" +
                           (who->host ? "Host operator, acting on " : "Signed in to ") + "<strong>" +
                           html_escape(domain) + "</strong> &middot; user " + std::to_string(who->user.user_id) +
                           " in realm <code>" + html_escape(who->user.realm) + "</code></p><p><strong>Roles</strong></p><ul>" +
                           roles + "</ul>" + apps +
                           "<form method=\"post\" action=\"/identity/sign-out\"><button>Sign out</button></form>"
                           "<p class=\"muted\"><a href=\"/\">Back to the site</a></p>"),
                  "");
  }
  finish(error_page(tenant, 404, request_id), R"({"why":"no such identity endpoint"})");
}

}  // namespace goober::host
