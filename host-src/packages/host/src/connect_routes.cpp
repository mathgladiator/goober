// Connections (design/machine/034.SpeakingHttp.md § 5): the host-served flow by which a person
// connects an account to a box's service, at /goober/connect/$box/$service. A pasted key is
// checked and sealed; an OAuth 2 authorization is run as a client for the person (state, PKCE),
// its code exchanged, and the tokens sealed. Either way the sealed value goes to the box's
// result route, as the person, and only the box keeps it. Also the spec store's tools.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/router/path.h"
#include "goober/signing/oauth1.h"
#include "goober/specs/specs.h"
#include "goober/sqlite/sqlite.h"
#include "goober/web/url.h"
#include "http_util.h"
#include "keys.h"
#include "network.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
using base::html_escape;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

namespace {

constexpr int64_t kStateTtlMs = 10 * 60 * 1000;

std::string random_hex(size_t n) {
  std::string raw(n, '\0');
  base::os_random_fill(std::span<uint8_t>(reinterpret_cast<uint8_t*>(raw.data()), raw.size()));
  return base::hex_encode(raw);
}

std::string arg(const bj::object& o, const char* k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

}  // namespace

void Host::handle_connect(const Request& req, const Finish& finish, const std::string& path, const std::string& domain,
                          const std::string& tenant) {
  std::string request_id = req.header("goober-request-id");
  auto cfg = config_.get();
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
  std::string origin = cfg->local ? cfg->base_url() : "https://" + domain;
  std::string redirect_uri = origin + "/goober/connect/callback";
  auto query = router::parse_query(req.query);
  auto get = [](const std::map<std::string, std::string>& m, const char* k) {
    auto it = m.find(k);
    return it == m.end() ? std::string() : it->second;
  };
  // The service a box declares with a connection, and the box itself.
  auto find = [&](const std::string& box, const std::string& service)
      -> std::pair<std::shared_ptr<const boxes::ActiveBox>, const manifest::ExternalService*> {
    auto active = boxes_->active();
    auto it = active->boxes.find(box);
    if (it == active->boxes.end()) return {nullptr, nullptr};
    bool serves = false;
    for (const auto& [t, d] : tenants_of(*cfg, it->second->manifest)) serves |= t == tenant;
    if (!serves) return {nullptr, nullptr};
    for (const auto& s : it->second->manifest.external) {
      if (s.name == service && !s.connect.kind.empty()) return {it->second, &s};
    }
    return {nullptr, nullptr};
  };
  // The sealed value, to the box's result route as the person, then the browser to return.
  auto hand_over = [&](const std::string& box, const manifest::ExternalService& svc, const SignedIn& who,
                       const std::string& plaintext, const std::string& kind, const std::string& ref, const std::string& scope,
                       const std::string& ret) {
    std::string err;
    std::string owner = std::to_string(who.user.user_id);
    // The origin the secret may go to is sealed with it (the service's base now), so neither a
    // box's call nor a later manifest can send it anywhere else.
    std::string with_origin = plaintext;
    if (auto u = web::parse_url(svc.base)) {
      std::string perr;
      if (auto pv = json::parse(plaintext, &perr); pv && pv->is_object()) {
        pv->as_object()["origin"] = u->origin();
        with_origin = json::dump(*pv);
      }
    }
    std::string sealed = seal_secret(tenant, svc.name, owner, with_origin, &err);
    if (sealed.empty()) {
      return finish(page(500, "Not connected", "<h1>Not connected</h1><p>" + html_escape(err) + "</p>"), R"({"connect":"seal_failed"})");
    }
    bj::object body{{"key", "connect:" + random_hex(8)}, {"service", svc.name}, {"kind", kind}, {"credential", sealed},
                    {"owner", owner}, {"ref", ref}, {"scope", scope}, {"label", svc.connect.label}};
    if (!deliver_result(tenant, box, svc.connect.result_route, body, who)) {
      return finish(page(502, "Not connected", "<h1>Not connected</h1><p>The site couldn't keep the connection. Nothing was "
                                                "saved; try again later.</p>"),
                    R"({"connect":"result_route_failed"})");
    }
    return finish(redirect(safe_return(ret)), json::dump(bj::object{{"connect", "connected"}, {"service", svc.name}, {"kind", kind}}));
  };

  if (path == "/goober/connect/callback") {
    std::string state = get(query, "state");
    sqlite::Db& db = connections_db();
    std::string box, service, user_id, ret, ref, verifier, state_domain;
    int64_t expires = 0;
    {
      std::lock_guard lock(vault_mu_);
      sqlite::Stmt s(db, "SELECT tenant, box, service, user_id, return_path, ref, verifier, domain, expires_ms FROM states WHERE state = ?");
      s.bind(1, state);
      if (!s.step() || s.text(0) != tenant) {
        return finish(page(400, "Not connected", "<h1>That link has expired</h1><p>Start connecting again from the site.</p>"),
                      R"({"connect":"bad_state"})");
      }
      box = s.text(1), service = s.text(2), user_id = s.text(3), ret = s.text(4), ref = s.text(5), verifier = s.text(6);
      state_domain = s.text(7);
      expires = s.int64(8);
      sqlite::Stmt d(db, "DELETE FROM states WHERE state = ?");  // used once
      d.bind(1, state).run();
    }
    if (expires < base::now_unix_ms()) {
      return finish(page(400, "Not connected", "<h1>That took too long</h1><p>Start connecting again from the site.</p>"),
                    R"({"connect":"state_expired"})");
    }
    // The person who started is the one finishing.
    auto who = browser_session(req, tenant);
    if (!who || who->host || std::to_string(who->user.user_id) != user_id) {
      return finish(page(403, "Not connected", "<h1>Sign in as the person who started</h1>"), R"({"connect":"other_person"})");
    }
    auto [ab, svc] = find(box, service);
    if (!svc || svc->connect.kind != "oauth2") {
      return finish(page(404, "Not connected", "<h1>That connection is gone</h1>"), R"({"connect":"no_service"})");
    }
    if (auto e = get(query, "error"); !e.empty()) {
      return finish(page(400, "Not connected", "<h1>Not connected</h1><p>" + html_escape(svc->name) + " said: " + html_escape(e) +
                                                   "</p><p><a href=\"" + html_escape(safe_return(ret)) + "\">Back</a></p>"),
                    json::dump(bj::object{{"connect", "provider_refused"}, {"error", e}}));
    }
    std::vector<std::pair<std::string, std::string>> form{{"grant_type", "authorization_code"}, {"code", get(query, "code")},
                                                          {"redirect_uri", redirect_uri}};
    if (svc->connect.pkce) form.emplace_back("code_verifier", verifier);
    std::string err;
    auto tokens = oauth2_token(tenant, *svc, form, &err);
    if (!tokens) {
      return finish(page(502, "Not connected", "<h1>Not connected</h1><p>" + html_escape(err) + "</p>"), R"({"connect":"exchange_failed"})");
    }
    int64_t now = base::now_unix_ms();
    bj::object secret{{"kind", "oauth2"}, {"access_token", arg(*tokens, "access_token")}, {"refresh_token", arg(*tokens, "refresh_token")},
                      {"scope", arg(*tokens, "scope")}, {"id", random_hex(12)}, {"gen", 0}};
    // expires_in (seconds), else an expires_at (epoch seconds, as Strava also sends); a value
    // that makes no sense is left out, and the token is treated as not expiring.
    constexpr double kTenYearsS = 10.0 * 365 * 24 * 3600;
    if (auto* ei = tokens->if_contains("expires_in"); ei && ei->is_number() && ei->to_number<double>() > 0 &&
                                                      ei->to_number<double>() <= kTenYearsS) {
      secret["expires_at"] = now + int64_t(ei->to_number<double>() * 1000);
    } else if (auto* ea = tokens->if_contains("expires_at"); ea && ea->is_number() && ea->to_number<double>() * 1000 > double(now) &&
                                                             ea->to_number<double>() <= double(now) / 1000 + kTenYearsS) {
      secret["expires_at"] = int64_t(ea->to_number<double>() * 1000);
    }
    if (arg(secret, "access_token").empty()) {
      return finish(page(502, "Not connected", "<h1>Not connected</h1><p>The provider gave no access token.</p>"),
                    R"({"connect":"no_token"})");
    }
    (void)state_domain;
    return hand_over(box, *svc, *who, json::dump(secret), "oauth2", ref, arg(secret, "scope"), ret);
  }

  // /goober/connect/$box/$service
  std::string rest = path.substr(std::string("/goober/connect/").size());
  auto slash = rest.find('/');
  if (slash == std::string::npos) return finish(error_page(tenant, 404, request_id), R"({"connect":"bad path"})");
  std::string box = rest.substr(0, slash), service = rest.substr(slash + 1);
  auto [ab, svc] = find(box, service);
  if (!svc) return finish(error_page(tenant, 404, request_id), R"({"connect":"no such connection"})");
  auto who = browser_session(req, tenant);
  if (!who || who->host) {
    return finish(redirect("/identity/sign-in?return=" + base::percent_encode(path + (req.query.empty() ? "" : "?" + req.query))),
                  R"({"connect":"sign_in_first"})");
  }
  std::string ret = safe_return(get(query, "return"));
  std::string ref = get(query, "ref").substr(0, 200);
  bool is_post = req.method == "post";
  if (is_post && !same_origin(req, origin_of(req))) return finish(error_page(tenant, 403, request_id), R"({"connect":"cross-origin"})");
  const auto& c = svc->connect;

  if (c.kind == "paste") {
    auto form = router::parse_query(req.body);
    std::string self = path;
    auto form_page = [&](const std::string& error) {
      return page(error.empty() ? 200 : 400, "Connect " + svc->name,
                  "<h1>Connect " + html_escape(svc->name) + "</h1>" +
                      (error.empty() ? "" : "<p class=\"error\">" + html_escape(error) + "</p>") +
                      "<form method=\"post\" action=\"" + html_escape(self) + "\"><label for=\"key\">" + html_escape(c.label) +
                      "</label><input id=\"key\" type=\"password\" name=\"key\" required autocomplete=\"off\">"
                      "<input type=\"hidden\" name=\"return\" value=\"" + html_escape(ret) + "\"><input type=\"hidden\" name=\"ref\" "
                      "value=\"" + html_escape(ref) + "\"><button>Connect</button></form>" +
                      (c.help.empty() ? "" : "<p class=\"muted\">" + html_escape(c.help) + "</p>") +
                      "<p class=\"muted\">The key is sealed by the site's host: the site keeps it, but only the host can read it, "
                      "and only to make the calls you'd expect.</p>");
    };
    if (!is_post) return finish(form_page(""), R"({"connect":"paste_form"})");
    std::string key = get(form, "key");
    ret = safe_return(get(form, "return"));
    ref = get(form, "ref").substr(0, 200);
    if (key.empty() || key.size() > 4096 || key.find_first_of("\r\n") != std::string::npos) {
      return finish(form_page("Paste the key as one line."), R"({"connect":"paste_bad"})");
    }
    // A check call with the key, when the manifest names one: a 401 or 403 means it didn't work.
    if (!c.check.empty()) {
      const manifest::ExternalMethod* m = nullptr;
      manifest::ExternalMethod by_op;
      if (auto it = svc->methods.find(c.check); it != svc->methods.end()) {
        m = &it->second;
      } else {
        by_op.name = "check";
        by_op.op = c.check;
        by_op.verb = "get";
        m = &by_op;
      }
      std::string err;
      auto call = build_httpx(tenant, bj::value(nullptr), domain, *svc, *m, "", bj::object{}, "", &err);
      Secret s;
      s.kind = "key";
      s.value = key;
      // The check must say yes: a key that can't be checked isn't sealed.
      if (call) {
        if (auto u = web::parse_url(svc->base)) s.origin = u->origin();
      }
      if (!call || !apply_secret(tenant, *svc, s, &*call, &err)) {
        std::cerr << "goober-host: connect " << box << "/" << service << ": the check can't be built: " << err << "\n";
        return finish(form_page("The key couldn't be checked (the site's check is misconfigured). Nothing was saved."),
                      R"({"connect":"paste_check_broken"})");
      }
      auto out = send_httpx(tenant, box, *svc, *m, *call, 10000, request_id, 0);
      if (out.status == 401 || out.status == 403) {
        return finish(form_page("That key didn't work: " + svc->name + " refused it."), R"({"connect":"paste_refused"})");
      }
      if (out.status < 200 || out.status >= 300) {
        return finish(form_page(svc->name + " couldn't be reached to check the key. Nothing was saved; try again soon."),
                      R"({"connect":"paste_check_failed"})");
      }
    }
    return hand_over(box, *svc, *who, json::dump(bj::object{{"kind", "key"}, {"value", key}}), "key", ref, "", ret);
  }

  // OAuth 2: to the provider, with state (and PKCE).
  std::string err, why;
  // The key store fills in {!tenant} itself.
  auto client_id = keys_->resolve_for(c.client_id, tenant, c.authorize, &why);
  if (!client_id) {
    return finish(page(500, "Not connected", "<h1>Not set up yet</h1><p>The site has no " + html_escape(svc->name) +
                                                 " app registered (" + html_escape(why) + ").</p>"),
                  R"({"connect":"no_client"})");
  }
  std::string state = random_hex(16), verifier = random_hex(32);
  {
    sqlite::Db& db = connections_db();
    std::lock_guard lock(vault_mu_);
    db.exec("DELETE FROM states WHERE expires_ms < " + std::to_string(base::now_unix_ms()));
    // At most 20 flows open per person: older ones are dropped.
    sqlite::Stmt cap(db, "DELETE FROM states WHERE user_id = ? AND state NOT IN (SELECT state FROM states WHERE user_id = ? "
                         "ORDER BY expires_ms DESC LIMIT 19)");
    cap.bind(1, std::to_string(who->user.user_id)).bind(2, std::to_string(who->user.user_id)).run();
    sqlite::Stmt s(db, "INSERT INTO states (state, tenant, box, service, user_id, return_path, ref, verifier, domain, expires_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    s.bind(1, state).bind(2, tenant).bind(3, box).bind(4, svc->name).bind(5, std::to_string(who->user.user_id)).bind(6, ret);
    s.bind(7, ref).bind(8, verifier).bind(9, domain).bind(10, base::now_unix_ms() + kStateTtlMs).run();
  }
  std::string scope;
  for (const auto& sc : c.scopes) scope += (scope.empty() ? "" : c.scope_separator) + sc;
  std::vector<std::pair<std::string, std::string>> q{{"response_type", "code"}, {"client_id", *client_id},
                                                     {"redirect_uri", redirect_uri}, {"state", state}};
  if (!scope.empty()) q.emplace_back("scope", scope);
  if (c.pkce) {
    auto digest = base::sha256(verifier);
    q.emplace_back("code_challenge", base::base64url_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size())));
    q.emplace_back("code_challenge_method", "S256");
  }
  for (const auto& [k, v] : c.extra) q.emplace_back(k, v);
  std::string url = c.authorize + (c.authorize.find('?') == std::string::npos ? "?" : "&");
  for (size_t i = 0; i < q.size(); ++i) url += (i ? "&" : "") + signing::rfc3986(q[i].first) + "=" + signing::rfc3986(q[i].second);
  return finish(redirect(url), json::dump(bj::object{{"connect", "to_provider"}, {"service", svc->name}}));
}

void Host::register_spec_tools() {
  specs_ = std::make_unique<specs::Store>(config_.get()->storage / "specs");
  using mcp::object_schema;
  using mcp::string_property;
  mcp_->add_tool({"spec_put",
                  "Stores an OpenAPI document (2.0, 3.0, 3.1, or 3.2, as JSON; convert YAML first) in the host's spec store and "
                  "answers its pin, name@ and the first 12 hex digits of its SHA-256, which a manifest's external service names "
                  "as \"spec\" (034 § 2). The same bytes give the same pin; a fixed spec is a new pin. Methods then import "
                  "operations by operationId or \"VERB /path\".",
                  object_schema({{"name", string_property("A name like strava or hevy: [a-z][a-z0-9_-]*.")},
                                 {"text", string_property("The spec's JSON.")}},
                                {"name", "text"}),
                  [this](const bj::object& args) {
                    std::string err;
                    auto pin = specs_->put(arg(args, "name"), arg(args, "text"), &err);
                    if (!pin) return ToolResult::fail(err);
                    auto spec = specs_->get(*pin, &err);
                    return ToolResult::ok(bj::object{{"pin", *pin},
                                                     {"format", spec ? spec->format : ""},
                                                     {"title", spec ? spec->title : ""},
                                                     {"operations", spec ? int64_t(spec->ops.size()) : 0}});
                  }});
  mcp_->add_tool({"spec_list", "The specs in the host's store: each pin, format, title, version, and how many operations.",
                  object_schema({}, {}), [this](const bj::object&) {
                    bj::array out;
                    for (const auto& s : specs_->list()) {
                      out.push_back(bj::object{{"pin", s.pin}, {"format", s.format}, {"title", s.title}, {"version", s.version},
                                               {"operations", int64_t(s.operations)}});
                    }
                    return ToolResult::ok(std::move(out));
                  }});
  mcp_->add_tool({"spec_get",
                  "One stored spec's operations: each operationId (or none), verb, path, parameters by place, and body types. "
                  "Filter with match (a substring of the id or path).",
                  object_schema({{"pin", string_property("name@sha12, from spec_put or spec_list.")},
                                 {"match", string_property("Only operations whose id or path holds this.")}},
                                {"pin"}),
                  [this](const bj::object& args) {
                    std::string err;
                    auto spec = specs_->get(arg(args, "pin"), &err);
                    if (!spec) return ToolResult::fail(err);
                    std::string match = arg(args, "match");
                    bj::array ops;
                    for (const auto& op : spec->ops) {
                      if (!match.empty() && op.id.find(match) == std::string::npos && op.path.find(match) == std::string::npos) continue;
                      bj::array params;
                      for (const auto& p : op.params) params.push_back(bj::object{{"name", p.name}, {"in", p.in}, {"required", p.required}});
                      bj::array types;
                      for (const auto& t : op.body_types) types.push_back(bj::value(t));
                      ops.push_back(bj::object{{"op", op.id.empty() ? op.ref() : op.id}, {"verb", op.verb}, {"path", op.path},
                                               {"params", std::move(params)}, {"body", std::move(types)}});
                      if (ops.size() >= 300) break;
                    }
                    return ToolResult::ok(bj::object{{"pin", arg(args, "pin")}, {"title", spec->title}, {"server", spec->server},
                                                     {"operations", std::move(ops)}});
                  }});
}

}  // namespace goober::host
