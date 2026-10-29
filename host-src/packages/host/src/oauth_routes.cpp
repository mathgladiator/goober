// The host-owned OAuth 2.1 endpoints (013.Identity.md § The implicit API; 999.Mobile.md
// § Signing in; design/machine/009.OAuth.md): metadata, the authorization code flow
// with a consent screen, the token and revocation endpoints, dynamic registration, and
// the device flow. Each tenant realm is its own issuer, at the domain's origin. The host realm
// is a second issuer under /oauth/operator, for agents reaching /goober-operations-mcp
// (design/machine/023.OperatorOAuth.md): only addresses in identity.operators sign in to it.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/router/path.h"
#include "http_util.h"

namespace goober::host {
namespace bj = boost::json;
using base::html_escape;
using server::http::Request;
using server::http::Response;

namespace {

Response json_response(int status, const bj::value& body) {
  Response res;
  res.status = status;
  res.body = json::dump(body);
  res.headers.emplace_back("content-type", "application/json");
  res.headers.emplace_back("cache-control", "no-store");
  res.headers.emplace_back("pragma", "no-cache");
  return res;
}

Response oauth_error(const identity::OAuthError& e) {
  // RFC 6749 §5.2: invalid_client is 401, everything else 400.
  return json_response(e.code == "invalid_client" ? 401 : 400,
                       bj::object{{"error", e.code}, {"error_description", e.description}});
}

std::string get(const std::map<std::string, std::string>& m, const char* k) {
  auto it = m.find(k);
  return it == m.end() ? std::string() : it->second;
}

// RFC 7636: an S256 challenge is base64url of 32 bytes, 43 characters.
bool is_s256_challenge(std::string_view c) {
  if (c.size() != 43) return false;
  for (char ch : c) {
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (!ok) return false;
  }
  return true;
}

std::string with_params(const std::string& uri, const std::vector<std::pair<std::string, std::string>>& params) {
  std::string out = uri + (uri.find('?') == std::string::npos ? "?" : "&");
  bool first = true;
  for (const auto& [k, v] : params) {
    if (!first) out += "&";
    first = false;
    out += k + "=" + base::percent_encode(v);
  }
  return out;
}

// Where a client comes from, in words a person can check (013 § Sign-in screen
// requirements; 999 § Signing in): the redirect host, the private-use scheme, or "an
// app on this computer" for a loopback redirect.
std::string where_from(const std::string& redirect_uri) {
  if (redirect_uri.starts_with("http://127.0.0.1") || redirect_uri.starts_with("http://[::1]")) {
    return "an app on this computer";
  }
  if (redirect_uri.starts_with("https://")) {
    auto rest = redirect_uri.substr(8);
    return rest.substr(0, rest.find('/'));
  }
  return "the app that opens " + redirect_uri.substr(0, redirect_uri.find(':') + 1) + " links";
}

// The CSP source that lets the consent form's redirect reach the client: browsers check
// form-action on the redirect too.
std::string form_action_for(const std::string& redirect_uri) {
  if (redirect_uri.starts_with("https://") || redirect_uri.starts_with("http://")) {
    auto scheme_end = redirect_uri.find("://") + 3;
    auto path = redirect_uri.find('/', scheme_end);
    return redirect_uri.substr(0, path);
  }
  return redirect_uri.substr(0, redirect_uri.find(':') + 1);
}

std::string roles_sentence(const std::vector<std::string>& roles) {
  if (roles.empty()) return "You hold no roles here yet.";
  std::string out = "It gets everything your roles allow: ";
  for (size_t i = 0; i < roles.size(); ++i) {
    if (i) out += ", ";
    out += "<code>" + html_escape(roles[i]) + "</code>";
  }
  return out + ".";
}

}  // namespace

std::string Host::origin_of(const Request& req) const {
  auto cfg = config_.get();
  bool secure = cfg->identity.cookie_secure && !cfg->local;
  return std::string(secure ? "https://" : "http://") + base::to_lower(req.header("host"));
}

std::optional<identity::Client> Host::oauth_client(const std::string& tenant, const std::string& client_id) {
  auto cfg = config_.get();
  auto it = cfg->identity.clients.find(client_id);
  auto stored = identity_->client(tenant, client_id);
  if (it == cfg->identity.clients.end()) return stored;
  // The config's clients exist in every realm; register (or update) this realm's copy.
  identity::Client want{client_id, it->second.name, it->second.redirect_uris, it->second.person, "config", 0};
  if (stored && stored->name == want.name && stored->redirect_uris == want.redirect_uris &&
      stored->person == want.person && stored->source == "config") {
    return stored;
  }
  std::string why;
  if (!identity_->put_client(tenant, want, base::now_unix_ms(), &why)) {
    std::cerr << "goober-host: identity.clients." << client_id << ": " << why << "\n";
    return std::nullopt;
  }
  return identity_->client(tenant, client_id);
}

void Host::handle_oauth(const Request& req, const Finish& finish, const std::string& path_in, const std::string& domain,
                        const std::string& tenant) {
  auto cfg = config_.get();
  // The operator issuer: the same endpoints under /oauth/operator, in the host realm.
  const std::string kOperatorBase = "/oauth/operator";
  bool op = path_in.starts_with(kOperatorBase + "/") || path_in == "/.well-known/oauth-authorization-server" + kOperatorBase ||
            path_in == "/.well-known/openid-configuration" + kOperatorBase;
  std::string path = path_in;
  if (path_in.starts_with(kOperatorBase + "/.well-known/") || !path_in.starts_with(kOperatorBase + "/")) {
    // RFC 8414 puts an issuer's metadata at /.well-known/oauth-authorization-server/$path;
    // some clients look under the issuer instead, or for OpenID's name. All are the same.
    if (op) path = "/.well-known/oauth-authorization-server";
    // The site's issuer too: clients that look for OpenID's name first (2026-10-29).
    if (!op && path_in == "/.well-known/openid-configuration") path = "/.well-known/oauth-authorization-server";
  } else {
    path = "/oauth/" + path_in.substr(kOperatorBase.size() + 1);
  }
  const std::string realm = op ? std::string(identity::kHostRealm) : tenant;
  const std::string base_path = op ? kOperatorBase : "/oauth";
  std::string request_id = req.header("goober-request-id");
  int64_t now = base::now_unix_ms();
  int64_t refresh_ms = cfg->identity.session_days * 24 * 3600 * 1000;
  std::string origin = origin_of(req);
  const std::string issuer = op ? origin + kOperatorBase : origin;
  bool is_get = req.method == "get" || req.method == "head";
  bool is_post = req.method == "post";
  auto log = [](const char* what) { return json::dump(bj::object{{"oauth", what}}); };

  if (path == "/.well-known/oauth-authorization-server" && is_get) {
    bj::object meta{
        {"issuer", issuer},
        {"authorization_endpoint", origin + base_path + "/authorize"},
        {"token_endpoint", origin + base_path + "/token"},
        {"revocation_endpoint", origin + base_path + "/revoke"},
        {"registration_endpoint", origin + base_path + "/register"},
        {"device_authorization_endpoint", origin + base_path + "/device"},
        {"response_types_supported", bj::array{"code"}},
        {"grant_types_supported",
         bj::array{"authorization_code", "refresh_token", "urn:ietf:params:oauth:grant-type:device_code"}},
        {"code_challenge_methods_supported", bj::array{"S256"}},
        {"token_endpoint_auth_methods_supported", bj::array{"none"}},
        {"revocation_endpoint_auth_methods_supported", bj::array{"none"}},
        {"authorization_response_iss_parameter_supported", true},
    };
    auto res = json_response(200, meta);
    res.headers.back() = {"cache-control", "no-cache"};
    return finish(std::move(res), log(op ? "operator_metadata" : "metadata"));
  }

  // The operator endpoint is a protected resource of the operator issuer (RFC 9728).
  if (path == "/.well-known/oauth-protected-resource" + std::string(kOperationsMcpPath) && is_get) {
    auto res = json_response(200, bj::object{{"resource", origin + std::string(kOperationsMcpPath)},
                                             {"authorization_servers", bj::array{bj::value(origin + kOperatorBase)}},
                                             {"bearer_methods_supported", bj::array{"header"}},
                                             {"resource_name", "goober operations on " + domain}});
    res.headers.back() = {"cache-control", "no-cache"};
    return finish(std::move(res), log("operator_protected_resource"));
  }

  // MCP authorization (RFC 9728): /mcp is a protected resource, signed in to by this realm.
  if ((path == "/.well-known/oauth-protected-resource" || path == "/.well-known/oauth-protected-resource/mcp") &&
      is_get) {
    auto res = json_response(200, bj::object{{"resource", origin + "/mcp"},
                                             {"authorization_servers", bj::array{bj::value(origin)}},
                                             {"bearer_methods_supported", bj::array{"header"}},
                                             {"resource_name", domain}});
    res.headers.back() = {"cache-control", "no-cache"};
    return finish(std::move(res), log("protected_resource"));
  }

  // ---- The authorization code flow: GET shows the consent screen, POST records the answer.
  if (path == "/oauth/authorize" && (is_get || is_post)) {
    auto p = is_get ? router::parse_query(req.query) : router::parse_query(req.body);
    std::string client_id = get(p, "client_id");
    std::string redirect_uri = get(p, "redirect_uri");
    std::string state = get(p, "state");
    auto bad = [&](const std::string& why) {
      // A bad client or redirect is never redirected to (RFC 6749 §4.1.2.1).
      return finish(identity_page(tenant, 400, "Can't sign in",
                                  "<h1>This sign-in link is broken</h1><p>" + html_escape(why) +
                                      "</p><p class=\"muted\">Go back to the app and try again.</p>"),
                    log("authorize_refused"));
    };
    auto to_client = [&](std::vector<std::pair<std::string, std::string>> params) {
      if (!state.empty()) params.emplace_back("state", state);
      params.emplace_back("iss", issuer);
      Response res;
      res.status = 303;
      res.headers.emplace_back("location", with_params(redirect_uri, params));
      res.headers.emplace_back("cache-control", "no-store");
      return res;
    };
    auto client = oauth_client(realm, client_id);
    if (!client) return bad("The app asking to sign in isn't registered here.");
    if (redirect_uri.empty() || !identity::redirect_allowed(*client, redirect_uri)) {
      return bad("The app asked to return somewhere it isn't registered to go.");
    }
    // A client that registered itself gets errors on a page, not a redirect: otherwise any
    // stranger could register evil.example and use this host as an open redirector
    // (RFC 9700 §4.11.2).
    bool redirect_errors = client->source != "dynamic";
    if (get(p, "response_type") != "code") {
      if (!redirect_errors) return bad("The app asked for something other than a sign-in code.");
      return finish(to_client({{"error", "unsupported_response_type"}}), log("authorize_error"));
    }
    std::string challenge = get(p, "code_challenge");
    if (get(p, "code_challenge_method") != "S256" || !is_s256_challenge(challenge)) {
      if (!redirect_errors) return bad("The app didn't use PKCE with S256.");
      return finish(to_client({{"error", "invalid_request"}, {"error_description", "PKCE with S256 is required"}}),
                    log("authorize_error"));
    }
    if (is_post && !user_submitted(req, origin)) {
      // What the browser sent, so a refused Allow can be traced to the browser that sent it.
      return finish(error_page(tenant, 403, request_id),
                    json::dump(bj::object{{"oauth", "authorize_cross_origin"},
                                          {"origin", req.header("origin")},
                                          {"sec_fetch", req.header("sec-fetch-site") + " " + req.header("sec-fetch-mode") +
                                                            " " + req.header("sec-fetch-dest") + " " +
                                                            req.header("sec-fetch-user")},
                                          {"user_agent", req.header("user-agent").substr(0, 200)}}));
    }
    // The operator issuer takes only a host operator's session; a tenant's, only a person's.
    auto who = browser_session(req, tenant);
    if (!who || who->host != op) {
      std::string back = base_path + "/authorize?" + (is_get ? req.query : req.body);
      Response res;
      res.status = 303;
      res.headers.emplace_back("location", std::string(op ? "/identity/sign-in/as-host" : "/identity/sign-in") +
                                               "?return=" + base::percent_encode(back));
      res.headers.emplace_back("cache-control", "no-store");
      return finish(std::move(res), log("authorize_sign_in"));
    }
    if (is_post) {
      // The answer is a page of our own that sends the browser on, with a button, never a bare
      // 303: Chrome checks every hop of a form post's redirects against form-action, so a client
      // whose callback redirects again (www.cursor.com does) is stopped without a word, and a
      // browser may drop an app launch it didn't see a click for. Either left the person looking
      // at an Allow button that did nothing (2026-10-28).
      auto handoff = [&](std::vector<std::pair<std::string, std::string>> params, bool allowed) {
        Response res = to_client(std::move(params));
        std::string target;
        for (const auto& [k, v] : res.headers) {
          if (k == "location") target = v;
        }
        std::string name = html_escape(client->name);
        Response page = identity_page(
            tenant, 200, allowed ? "Allowed" : "Not allowed",
            std::string(allowed ? "<h1>Allowed</h1><p>Opening <strong>" + name + "</strong> to finish signing in.</p>"
                                : "<h1>Not allowed</h1><p>Telling <strong>" + name + "</strong> you said no.</p>") +
                "<p><a class=\"button\" href=\"" + html_escape(target) + "\">Open " + name + "</a></p>"
                "<p class=\"muted\">If nothing opens, press the button. Once the app says it's connected, you can "
                "close this tab.</p>",
            "", false);
        page.headers.emplace_back("refresh", "0;url=" + target);
        return page;
      };
      bj::object extra{{"oauth", "authorize_allowed"},
                       {"client_id", client_id},
                       {"user_id", std::to_string(who->user.user_id)},
                       {"to", form_action_for(redirect_uri)}};
      if (get(p, "decision") != "allow") {
        extra["oauth"] = "authorize_denied";
        return finish(handoff({{"error", "access_denied"}}, false), json::dump(extra));
      }
      std::string code = identity_->issue_code(realm, client_id, who->user.user_id, redirect_uri, challenge, now);
      mail_new_grant(*who, *client, domain, where_from(redirect_uri));
      return finish(handoff({{"code", code}}, true), json::dump(extra));
    }
    std::string hidden;
    for (const char* k : {"response_type", "client_id", "redirect_uri", "code_challenge", "code_challenge_method",
                          "state", "scope"}) {
      auto v = get(p, k);
      if (!v.empty()) {
        hidden += "<input type=\"hidden\" name=\"" + std::string(k) + "\" value=\"" + html_escape(v) + "\">";
      }
    }
    std::string kind =
        op ? "<p><strong>" + html_escape(client->name) + "</strong> is an <strong>agent</strong>" +
                 (client->source == "dynamic" ? " that registered itself, so nobody here has checked it" : "") +
                 ". It will act <strong>as a host operator</strong>: everything the operator tools can do on this "
                 "host, on every site. It can change pages, boxes, data, keys, and backups.</p>"
        : client->person
            ? "<p><strong>" + html_escape(client->name) + "</strong> is this site's own app. It will act <strong>as "
                  "you</strong>, the way you do in a browser.</p>"
            : "<p><strong>" + html_escape(client->name) + "</strong> is an <strong>agent</strong>" +
                  (client->source == "dynamic" ? " that registered itself, so nobody here has checked it" : "") +
                  ". It will act <strong>for you</strong>, but only on what this site lets agents do.</p>";
    std::string body =
        "<h1>Allow " + html_escape(client->name) + "?</h1>" +
        "<p class=\"muted\">Signing in to <strong>" + html_escape(op ? "operate this host" : domain) + "</strong> as <strong>" +
        html_escape(who->user.email) + "</strong></p>" + kind + "<p>It comes from <strong>" +
        html_escape(where_from(redirect_uri)) + "</strong>" +
        (client->source == "dynamic" ? " <span class=\"error\">(unverified; it registered itself on " +
                                           base::format_utc_ms(client->created_at).substr(0, 16) + " UTC)</span>"
                                     : "") +
        ".</p><p>" +
        roles_sentence(who->roles) +
        "</p><p class=\"muted\">Only allow it if you just started this sign-in yourself. You can remove it later "
        "from <a href=\"/identity/account\">your account</a>.</p>"
        "<form method=\"post\" action=\"" + base_path + "/authorize\">" +
        hidden +
        "<button name=\"decision\" value=\"allow\">Allow</button>"
        "<button name=\"decision\" value=\"deny\" class=\"secondary\">Don't allow</button></form>";
    return finish(identity_page(tenant, 200, "Allow " + client->name + "?", body, "", false),
                  log("consent"));
  }

  // ---- The token endpoint: codes, refresh tokens, and device codes.
  if (path == "/oauth/token" && is_post) {
    auto p = router::parse_query(req.body);
    std::string grant = get(p, "grant_type");
    std::string client_id = get(p, "client_id");
    identity::OAuthError e;
    std::optional<identity::Tokens> t;
    if (client_id.empty() || !oauth_client(realm, client_id)) {
      e = {"invalid_client", "unknown client"};
    } else if (grant == "authorization_code") {
      t = identity_->exchange_code(realm, client_id, get(p, "code"), get(p, "redirect_uri"), get(p, "code_verifier"),
                                   now, refresh_ms, &e);
    } else if (grant == "refresh_token") {
      t = identity_->refresh(realm, client_id, get(p, "refresh_token"), now, refresh_ms, &e);
    } else if (grant == "urn:ietf:params:oauth:grant-type:device_code") {
      t = identity_->poll_device(realm, client_id, get(p, "device_code"), now, refresh_ms, &e);
    } else {
      e = {"unsupported_grant_type", "use authorization_code, refresh_token, or the device_code grant"};
    }
    bj::object extra{{"oauth", op ? "operator_token" : "token"}, {"grant", grant}, {"client_id", client_id}};
    if (!t) {
      extra["error"] = e.code;
      extra["why"] = e.description;
      return finish(oauth_error(e), json::dump(extra));
    }
    return finish(json_response(200, bj::object{{"access_token", t->access_token},
                                                {"token_type", "Bearer"},
                                                {"expires_in", t->expires_in_s},
                                                {"refresh_token", t->refresh_token}}),
                  json::dump(extra));
  }

  if (path == "/oauth/revoke" && is_post) {
    auto p = router::parse_query(req.body);
    identity_->revoke_token(realm, get(p, "client_id"), get(p, "token"), now);
    // RFC 7009 §2.2: 200 whether or not the token was valid.
    return finish(json_response(200, bj::object{}), log("revoke"));
  }

  // ---- Dynamic registration (RFC 7591): agents introduce themselves.
  if (path == "/oauth/register" && is_post) {
    std::string err;
    auto doc = json::parse(req.body, &err);
    if (!doc || !doc->is_object()) {
      return finish(json_response(400, bj::object{{"error", "invalid_client_metadata"},
                                                  {"error_description", "the body must be a JSON object"}}),
                    log("register_refused"));
    }
    const auto& o = doc->as_object();
    std::string name = o.contains("client_name") && o.at("client_name").is_string()
                           ? std::string(o.at("client_name").as_string())
                           : "";
    std::vector<std::string> uris;
    if (auto* u = o.if_contains("redirect_uris"); u && u->is_array()) {
      for (const auto& v : u->as_array()) {
        if (v.is_string()) uris.emplace_back(v.as_string());
      }
    }
    std::string why;
    auto c = identity_->register_client(realm, name, uris, now, &why);
    if (!c) {
      bj::array asked;
      for (const auto& u : uris) asked.push_back(bj::value(u.substr(0, 200)));
      return finish(json_response(400, bj::object{{"error", "invalid_client_metadata"}, {"error_description", why}}),
                    json::dump(bj::object{{"oauth", "register_refused"}, {"why", why}, {"redirect_uris", std::move(asked)}}));
    }
    bj::array out_uris;
    for (const auto& u : c->redirect_uris) out_uris.push_back(bj::value(u));
    return finish(json_response(201, bj::object{{"client_id", c->client_id},
                                                {"client_name", c->name},
                                                {"redirect_uris", std::move(out_uris)},
                                                {"client_id_issued_at", now / 1000},
                                                {"token_endpoint_auth_method", "none"},
                                                {"grant_types", bj::array{"authorization_code", "refresh_token",
                                                                          "urn:ietf:params:oauth:grant-type:device_code"}},
                                                {"response_types", bj::array{"code"}}}),
                  json::dump(bj::object{{"oauth", "register"}, {"client_id", c->client_id}}));
  }

  // ---- The device flow (RFC 8628): the agent starts, the person types the code.
  if (path == "/oauth/device" && is_post) {
    auto p = router::parse_query(req.body);
    std::string client_id = get(p, "client_id");
    identity::OAuthError e;
    std::optional<identity::DeviceStart> d;
    if (!oauth_client(realm, client_id)) {
      e = {"invalid_client", "unknown client"};
    } else {
      d = identity_->start_device(realm, client_id, now, &e);
    }
    if (!d) return finish(oauth_error(e), log("device_refused"));
    // No verification_uri_complete: the person types the code on a page they reached
    // themselves (013 § Sign-in screen requirements).
    return finish(json_response(200, bj::object{{"device_code", d->device_code},
                                                {"user_code", d->user_code},
                                                {"verification_uri", origin + base_path + "/device/verify"},
                                                {"expires_in", d->expires_in_s},
                                                {"interval", d->interval_s}}),
                  json::dump(bj::object{{"oauth", "device_start"}, {"client_id", client_id}}));
  }

  if (path == "/oauth/device/verify" && (is_get || is_post)) {
    if (is_post && !user_submitted(req, origin)) return finish(error_page(tenant, 403, request_id), log("device_cross_origin"));
    auto who = browser_session(req, tenant);
    if (!who || who->host != op) {
      Response res;
      res.status = 303;
      res.headers.emplace_back("location", std::string(op ? "/identity/sign-in/as-host" : "/identity/sign-in") +
                                               "?return=" + base::percent_encode(base_path + "/device/verify"));
      res.headers.emplace_back("cache-control", "no-store");
      return finish(std::move(res), log("device_sign_in"));
    }
    auto form = router::parse_query(req.body);
    auto enter = [&](const std::string& error) {
      return identity_page(
          tenant, error.empty() ? 200 : 400, "Connect a device",
          "<h1>Connect a device</h1><p class=\"muted\">Signed in to <strong>" + html_escape(domain) +
              "</strong> as <strong>" + html_escape(who->user.email) + "</strong></p>" +
              (error.empty() ? "" : "<p class=\"error\">" + html_escape(error) + "</p>") +
              "<p>Type the code the device or agent is showing you.</p>"
              "<form method=\"post\" action=\"" + base_path + "/device/verify\"><label for=\"code\">Code</label>"
              "<input id=\"code\" type=\"text\" name=\"user_code\" required autofocus autocomplete=\"off\" "
              "placeholder=\"WDJB-MJHT\" maxlength=\"9\"><input type=\"hidden\" name=\"step\" value=\"check\">"
              "<button>Continue</button></form><p class=\"muted\">Only continue if you started this sign-in "
              "yourself. Never type a code somebody sent you.</p>");
    };
    if (is_get) return finish(enter(""), log("device_page"));
    std::string user_code = get(form, "user_code");
    auto client = identity_->device_client(realm, user_code, now);
    if (get(form, "step") == "check") {
      if (!client) return finish(enter("That code isn't waiting for anyone. Check it, or start again."), log("device_bad_code"));
      std::string body =
          "<h1>Allow " + html_escape(client->name) + "?</h1><p class=\"muted\">Signing in to <strong>" +
          html_escape(domain) + "</strong> as <strong>" + html_escape(who->user.email) + "</strong></p><p><strong>" +
          html_escape(client->name) + "</strong> is an <strong>agent</strong> on a device without a browser" +
          (client->source == "dynamic" ? " <span class=\"error\">(unverified: it registered itself)</span>" : "") +
          (op ? ". It will act <strong>as a host operator</strong>: everything the operator tools can do on this host."
              : ". It will act <strong>for you</strong>, but only on what this site lets agents do.") +
          "</p><p>" + roles_sentence(who->roles) +
          "</p><form method=\"post\" action=\"" + base_path + "/device/verify\"><input type=\"hidden\" name=\"user_code\" "
          "value=\"" + html_escape(user_code) + "\"><input type=\"hidden\" name=\"step\" value=\"decide\">"
          "<button name=\"decision\" value=\"allow\">Allow</button>"
          "<button name=\"decision\" value=\"deny\" class=\"secondary\">Don't allow</button></form>";
      return finish(identity_page(tenant, 200, "Allow " + client->name + "?", body, "", false), log("device_consent"));
    }
    bool allow = get(form, "decision") == "allow";
    std::string why;
    if (!identity_->decide_device(realm, user_code, who->user.user_id, allow, now, &why)) {
      return finish(enter(why), log("device_bad_code"));
    }
    if (allow && client) mail_new_grant(*who, *client, domain, "a device without a browser");
    return finish(identity_page(tenant, 200, allow ? "Device connected" : "Device refused",
                                allow ? "<h1>Done</h1><p>The device is connected. You can close this page, and remove "
                                        "it any time from <a href=\"/identity/account\">your account</a>.</p>"
                                      : "<h1>Refused</h1><p>The device gets nothing.</p>"),
                  json::dump(bj::object{{"oauth", allow ? "device_allowed" : "device_denied"},
                                        {"user_id", std::to_string(who->user.user_id)}}));
  }

  finish(error_page(tenant, is_get || is_post ? 404 : 405, request_id), log("no_such_endpoint"));
}

void Host::mail_new_grant(const SignedIn& who, const identity::Client& client, const std::string& domain,
                          const std::string& from) {
  // No link in it: a link in an email is exactly what a phisher would send (013).
  identity::Mail m{
      who.user.email, "A new " + std::string(client.person ? "app" : "agent") + " can act for you on " + domain,
      std::string(client.person ? "An app" : "An agent") + " was just allowed to act " +
          (client.person ? "as you" : "for you") + " on " + domain + ": " + client.name + ", from " + from +
          (client.source == "dynamic" ? " (it registered itself, so nobody has checked it)" : "") +
          ".\n\nIf that was you, there is nothing to do. If it wasn't, open " + domain +
          " yourself, go to your account page, and remove it.\n"};
  m.realm = who.user.realm;
  m.template_name = "new-grant";
  m.vars = bj::object{{"kind", client.person ? "app" : "agent"}, {"acting", client.person ? "as you" : "for you"},
                      {"client", client.name},                   {"from", from},
                      {"unchecked", client.source == "dynamic"},  {"site", domain}};
  send_mail(m);
}

}  // namespace goober::host
