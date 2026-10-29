// Progressive web apps (design/machine/782.PWA.md): /goober/pwa/$app/ serves each app's
// manifest, service worker, and registration script, and takes push subscriptions; the
// pwa_* tools write and read apps; boxes send Web Push through a "push" service, delivered
// after commit through the outbox, signed with the host's VAPID key and encrypted per
// subscription (RFC 8291, 8292).

#include <iostream>
#include <set>

#include "build_id.h"
#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/site/pwa.h"
#include "goober/sqlite/sqlite.h"
#include "goober/webpush/webpush.h"
#include "http_util.h"
#include "keys.h"
#include "network.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;
using server::http::Request;
using server::http::Response;

namespace {

constexpr const char* kVapidKeyName = "vapid_private_key";
// The browsers' own push services. Subscriptions naming anything else are refused, so the
// host never posts to an address a visitor chose (config pwa.push-hosts adds more).
const char* const kPushHosts[] = {"fcm.googleapis.com", "updates.push.services.mozilla.com", "*.push.apple.com",
                                  "*.notify.windows.com"};
constexpr int kSubscriptionsPerUser = 20;
constexpr size_t kTopicsMax = 20;
constexpr int64_t kSendFanoutMax = 10000;

bool host_fits(std::string_view pattern, std::string_view host) {
  if (pattern.starts_with("*.")) return host.size() > pattern.size() - 1 && host.ends_with(pattern.substr(1));
  return pattern == host;
}

std::string text_of(const bj::object& o, std::string_view k) {
  auto* v = o.if_contains(k);
  return v && v->is_string() ? std::string(v->as_string()) : std::string();
}

bool is_topic(std::string_view t) {
  if (t.empty() || t.size() > 64) return false;
  for (char c : t) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

bool local_path(std::string_view u) {
  return !u.empty() && u.front() == '/' && !u.starts_with("//") && u.find_first_of(" \"'<>\\\r\n\t") == std::string_view::npos;
}

// Who a push goes to: exactly one of {user_id}, {topic}, {subscription}, {everyone: true}.
struct Target {
  int64_t user_id = 0, subscription = 0;
  std::string topic;
};
std::optional<Target> target_of(const bj::object& to, std::string* why) {
  Target t;
  auto id = [](const bj::value& v) -> int64_t {
    if (v.is_int64() && v.as_int64() > 0) return v.as_int64();
    if (v.is_string() && !v.as_string().empty() && v.as_string().size() < 19 &&
        std::string_view(v.as_string()).find_first_not_of("0123456789") == std::string_view::npos) {
      return std::stoll(std::string(v.as_string()));
    }
    return 0;
  };
  if (to.size() != 1) return *why = "to is one of {user_id}, {topic}, {subscription}, or {everyone: true}", std::nullopt;
  if (auto* u = to.if_contains("user_id")) {
    if (!(t.user_id = id(*u))) return *why = "to.user_id is a user's id", std::nullopt;
  } else if (auto* s = to.if_contains("subscription")) {
    if (!(t.subscription = id(*s))) return *why = "to.subscription is a subscription's id", std::nullopt;
  } else if (auto* tp = to.if_contains("topic")) {
    if (!tp->is_string() || !is_topic(tp->as_string())) return *why = "to.topic is 1 to 64 of a-z, 0-9, _ and -", std::nullopt;
    t.topic = std::string(tp->as_string());
  } else if (auto* e = to.if_contains("everyone"); !(e && e->is_bool() && e->as_bool())) {
    return *why = "to is one of {user_id}, {topic}, {subscription}, or {everyone: true}", std::nullopt;
  }
  return t;
}

Response json_out(int status, const bj::value& v) {
  Response res;
  res.status = status;
  res.headers.emplace_back("content-type", "application/json");
  res.headers.emplace_back("cache-control", "no-store");
  res.body = json::dump(v);
  return res;
}

}  // namespace

// What a notification shows: {title, body, url, icon, badge, tag, data}, at most 3 KiB in all.
std::optional<std::string> push_payload(const bj::object& r, std::string* why) {
  bj::object p;
  for (const char* k : {"title", "body", "url", "icon", "badge", "tag"}) {
    auto* v = r.if_contains(k);
    if (!v) continue;
    if (!v->is_string()) return *why = std::string(k) + " is text", std::nullopt;
    std::string s(v->as_string());
    if ((std::string_view(k) == "url" || std::string_view(k) == "icon" || std::string_view(k) == "badge") && !local_path(s)) {
      return *why = std::string(k) + " is a path on this site, like /app/orders/7", std::nullopt;
    }
    p[k] = base::valid_utf8(s);
  }
  if (auto* d = r.if_contains("data")) p["data"] = *d;
  if (!p.contains("title") && !p.contains("body")) return *why = "a push needs a title or a body", std::nullopt;
  std::string out = json::dump(p);
  if (out.size() > webpush::kMaxPlaintext) {
    return *why = "a push is at most " + std::to_string(webpush::kMaxPlaintext) + " bytes of JSON; send a url and fetch the rest",
           std::nullopt;
  }
  return out;
}

std::optional<webpush::Key> Host::vapid_key(std::string* error) {
  std::lock_guard lock(vapid_mu_);
  std::string err;
  if (auto stored = keys_->resolve(std::string("keys/system/") + kVapidKeyName, "")) {
    if (auto raw = webpush::b64url_decode(*stored)) return webpush::key_from_private(*raw, error);
  }
  // The first app with push makes the host's key, once; it lives in the key store with the rest.
  auto key = webpush::generate_key(error);
  if (!key) return std::nullopt;
  if (!keys_->set("system", "", kVapidKeyName, webpush::b64url(key->private_raw), base::now_unix_ms(), error)) {
    return std::nullopt;
  }
  return key;
}

std::unique_ptr<sqlite::Db> Host::open_push_db() {
  auto dir = config_.get()->storage / "pwa";
  fs::create_directories(dir);
  auto db = std::make_unique<sqlite::Db>(dir / "push.db");
  db->exec(
      "CREATE TABLE IF NOT EXISTS subscriptions (id INTEGER PRIMARY KEY AUTOINCREMENT, tenant TEXT NOT NULL,"
      " app TEXT NOT NULL, user_id INTEGER, endpoint TEXT NOT NULL UNIQUE, p256dh TEXT NOT NULL, auth TEXT NOT NULL,"
      " topics TEXT NOT NULL, created_ms INTEGER NOT NULL, last_ok_ms INTEGER, failures INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS subscriptions_user ON subscriptions (tenant, user_id);"
      "CREATE TABLE IF NOT EXISTS sent (key TEXT NOT NULL, subscription INTEGER NOT NULL, at_ms INTEGER NOT NULL,"
      " PRIMARY KEY (key, subscription));");
  return db;
}

bool Host::push_endpoint_allowed(const std::string& endpoint, bool* loopback, std::string* why) const {
  auto cfg = config_.get();
  *loopback = false;
  bool https = endpoint.starts_with("https://");
  bool http = endpoint.starts_with("http://");
  if (!https && !http) return *why = "a push endpoint is an https URL", false;
  std::string rest = endpoint.substr(https ? 8 : 7);
  std::string authority = rest.substr(0, rest.find_first_of("/?#"));
  std::string host = base::to_lower(authority.substr(0, authority.rfind(':') == std::string::npos ? authority.size() : authority.rfind(':')));
  if (endpoint.size() > 2048 || host.empty()) return *why = "not a push endpoint", false;
  for (const auto& p : cfg->pwa.push_hosts) {
    if (host_fits(p, host)) {
      // A stand-in on this machine (tests, local play) is the only plain http allowed.
      *loopback = host == "127.0.0.1" || host == "localhost";
      if (http && !(*loopback && cfg->local)) return *why = "a push endpoint is an https URL", false;
      return true;
    }
  }
  if (!https) return *why = "a push endpoint is an https URL", false;
  for (const char* p : kPushHosts) {
    if (host_fits(p, host)) return true;
  }
  return *why = host + " isn't a browser's push service (config pwa.push-hosts adds one)", false;
}

std::string Host::vapid_contact() const {
  auto cfg = config_.get();
  if (!cfg->pwa.push_contact.empty()) return cfg->pwa.push_contact;
  std::string from = cfg->identity.from_address();
  if (from.find('@') != std::string::npos && !from.ends_with("@localhost")) return "mailto:" + from;
  if (cfg->public_url.starts_with("https://")) return cfg->public_url;
  return "mailto:postmaster@" + cfg->platform_domain;
}

// Sends one payload to the subscriptions `to` names, skipping those this key already reached.
// Gone subscriptions (404, 410) are removed. A 503 (retry) while any send may still pass and a key keeps retries from repeating.
HttpOut Host::deliver_push(const std::string& tenant, const std::string& app, const bj::object& to,
                           const std::string& payload, const bj::object& options, const std::string& key, int64_t now_ms,
                           bj::object* report) {
  HttpOut out;
  std::string err;
  auto vapid = vapid_key(&err);
  if (!vapid) {
    out.status = 500;
    out.error = "no VAPID key: " + err;
    return out;
  }
  std::string why_target;
  auto target = target_of(to, &why_target);
  if (!target) {
    out.status = 400;
    out.error = why_target;
    return out;
  }
  auto db = open_push_db();
  std::string sql = "SELECT id, endpoint, p256dh, auth, topics, user_id FROM subscriptions WHERE tenant = ? AND (? = '' OR app = ?)";
  const std::string& topic = target->topic;
  if (target->user_id) sql += " AND user_id = ?";
  if (target->subscription) sql += " AND id = ?";
  sql += " ORDER BY id LIMIT " + std::to_string(kSendFanoutMax);
  struct Sub {
    int64_t id;
    std::string endpoint, p256dh, auth;
  };
  std::vector<Sub> subs;
  {
    sqlite::Stmt s(*db, sql);
    s.bind(1, tenant).bind(2, app).bind(3, app);
    int i = 4;
    if (target->user_id) s.bind(i++, target->user_id);
    if (target->subscription) s.bind(i++, target->subscription);
    while (s.step()) {
      if (!topic.empty()) {
        auto topics = json::parse(s.text(4), &err);
        bool has = false;
        if (topics && topics->is_array()) {
          for (const auto& t : topics->as_array()) has |= t.is_string() && t.as_string() == topic;
        }
        if (!has) continue;
      }
      subs.push_back({s.int64(0), s.text(1), s.text(2), s.text(3)});
    }
  }
  int64_t ttl = 86400;
  if (auto* t = options.if_contains("ttl"); t && t->is_int64()) ttl = std::clamp<int64_t>(t->as_int64(), 0, 28 * 86400);
  std::string urgency = text_of(options, "urgency");
  std::string collapse = text_of(options, "collapse");
  int sent = 0, gone = 0, retry = 0, refused = 0, skipped = 0;
  auto cfg = config_.get();
  for (const auto& sub : subs) {
    if (!key.empty()) {
      sqlite::Stmt seen(*db, "SELECT 1 FROM sent WHERE key = ? AND subscription = ?");
      seen.bind(1, key).bind(2, sub.id);
      if (seen.step()) {
        ++skipped;
        continue;
      }
    }
    bool loopback = false;
    std::string why;
    auto p256dh = webpush::b64url_decode(sub.p256dh);
    auto auth = webpush::b64url_decode(sub.auth);
    std::optional<std::string> body;
    if (push_endpoint_allowed(sub.endpoint, &loopback, &why) && p256dh && auth) {
      body = webpush::encrypt(payload, *p256dh, *auth, &why);
    }
    std::string rest = sub.endpoint.substr(sub.endpoint.find("://") + 3);
    std::string audience = sub.endpoint.substr(0, sub.endpoint.find("://") + 3) + rest.substr(0, rest.find_first_of("/?#"));
    auto authorization = body ? webpush::vapid_authorization(*vapid, audience, vapid_contact(), now_ms / 1000 + 12 * 3600, &why)
                              : std::nullopt;
    if (!body || !authorization) {
      ++refused;
      continue;
    }
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", *authorization}, {"Content-Encoding", "aes128gcm"}, {"TTL", std::to_string(ttl)}};
    if (urgency == "very-low" || urgency == "low" || urgency == "normal" || urgency == "high") headers.emplace_back("Urgency", urgency);
    if (!collapse.empty()) headers.emplace_back("Topic", collapse);
    int64_t started = base::monotonic_us();
    auto reply = send_http("POST", sub.endpoint, headers, *body, "application/octet-stream", loopback && cfg->local,
                           cfg->storage / "tmp", 10000);
    egress_log(tenant, "", "push", "send", "POST", audience, reply.status, reply.error, base::monotonic_us() - started,
               body->size(), reply.body.size(), "", 0);
    if (reply.error.empty() && reply.status >= 200 && reply.status < 300) {
      ++sent;
      sqlite::Stmt ok(*db, "UPDATE subscriptions SET last_ok_ms = ?, failures = 0 WHERE id = ?");
      ok.bind(1, now_ms).bind(2, sub.id);
      ok.run();
      if (!key.empty()) {
        sqlite::Stmt mark(*db, "INSERT OR IGNORE INTO sent VALUES (?, ?, ?)");
        mark.bind(1, key).bind(2, sub.id).bind(3, now_ms);
        mark.run();
      }
    } else if (reply.status == 404 || reply.status == 410) {
      // The browser dropped it (unsubscribed, uninstalled): so does the host.
      ++gone;
      sqlite::Stmt del(*db, "DELETE FROM subscriptions WHERE id = ?");
      del.bind(1, sub.id);
      del.run();
    } else {
      bool later = reply.status == 0 || reply.status == 429 || reply.status >= 500;
      later ? ++retry : ++refused;
      sqlite::Stmt f(*db, "UPDATE subscriptions SET failures = failures + 1 WHERE id = ?");
      f.bind(1, sub.id);
      f.run();
    }
  }
  // Old marks only need to outlast the outbox's retries.
  sqlite::Stmt trim(*db, "DELETE FROM sent WHERE at_ms < ?");
  trim.bind(1, now_ms - 7LL * 86400 * 1000);
  trim.run();
  if (report) {
    *report = bj::object{{"subscriptions", int64_t(subs.size())}, {"sent", sent},     {"gone", gone},
                         {"retry", retry},                         {"refused", refused}, {"already_sent", skipped}};
  }
  // Retried while any send may still pass: those already reached are skipped by the key.
  bool worth_retrying = retry > 0 && !key.empty();
  out.status = worth_retrying ? 503 : 201;
  if (worth_retrying) out.error = std::to_string(retry) + " push service(s) failed for now";
  out.body = json::dump(bj::object{{"sent", sent}, {"gone", gone}, {"retry", retry}, {"refused", refused}});
  return out;
}

HttpOut Host::deliver_outbox_push(const std::string& tenant, const std::string& body, const std::string& key, int64_t now_ms) {
  std::string err;
  auto v = json::parse(body, &err);
  HttpOut out;
  if (!v || !v->is_object()) {
    out.status = 400;
    out.error = "the push element isn't readable";
    return out;
  }
  const auto& o = v->as_object();
  auto* to = o.if_contains("to");
  auto* options = o.if_contains("options");
  return deliver_push(tenant, text_of(o, "app"), to && to->is_object() ? to->as_object() : bj::object{}, text_of(o, "payload"),
                      options && options->is_object() ? options->as_object() : bj::object{}, key, now_ms, nullptr);
}

std::string Host::push_call(CallContext& ctx, const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                            const bj::value& request) {
  auto fail = [](std::string why) { return json::dump(bj::object{{"error", std::move(why)}}); };
  const char* kShape =
      "send takes {to: {user_id} | {topic} | {everyone: true}, app?, title?, body?, url?, icon?, badge?, tag?, data?, "
      "ttl?, urgency?, collapse?}";
  if (!request.is_object()) return fail(kShape);
  const auto& r = request.as_object();
  static const std::set<std::string, std::less<>> kFields = {"to",  "app", "title", "body",    "url",     "icon",
                                                             "badge", "tag", "data", "ttl", "urgency", "collapse"};
  for (const auto& kv : r) {
    if (!kFields.count(kv.key())) return fail(std::string(kShape) + "; \"" + std::string(kv.key()) + "\" isn't one");
  }
  auto* to = r.if_contains("to");
  if (!to || !to->is_object()) return fail(kShape);
  std::string why;
  auto t = target_of(to->as_object(), &why);
  if (!t) return fail(why);
  bj::object target;
  std::string describe = "everyone";
  if (t->user_id) target["user_id"] = t->user_id, describe = "user/" + std::to_string(t->user_id);
  if (t->subscription) target["subscription"] = t->subscription, describe = "subscription/" + std::to_string(t->subscription);
  if (!t->topic.empty()) target["topic"] = t->topic, describe = "topic/" + t->topic;
  if (target.empty()) target["everyone"] = true;
  std::string app = text_of(r, "app");
  if (!app.empty() && !site::is_app_name(app)) return fail("app is an app's name, from pwa_list");
  auto payload = push_payload(r, &why);
  if (!payload) return fail(why);
  bj::object options;
  if (auto* v = r.if_contains("ttl")) {
    if (!v->is_int64() || v->as_int64() < 0) return fail("ttl is seconds a push service may hold it, 0 to 2419200");
    options["ttl"] = v->as_int64();
  }
  if (auto u = text_of(r, "urgency"); !u.empty()) {
    if (u != "very-low" && u != "low" && u != "normal" && u != "high") return fail("urgency is very-low, low, normal, or high");
    options["urgency"] = u;
  }
  if (auto c = text_of(r, "collapse"); !c.empty()) {
    if (c.size() > 32 || c.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) {
      return fail("collapse is 1 to 32 of A-Z, a-z, 0-9, - and _ (a newer push with it replaces an older one)");
    }
    options["collapse"] = c;
  }
  if (!outbox_quota_take(ctx.tenant, svc.name, &why)) return fail(why);
  OutboxElement e;
  e.service = svc.name;
  e.method = m.name;
  e.verb = "PUSH";
  e.url = "push:" + describe;
  e.body = json::dump(bj::object{{"app", app}, {"to", target}, {"payload", *payload}, {"options", options}});
  e.idempotency_key = base::sha256_hex("goober-outbox\n" + ctx.request_id + "\n" + std::to_string(ctx.pending.size())).substr(0, 32);
  e.attempts = m.attempts;
  e.backoff_ms = m.backoff_ms;
  e.within_ms = m.within_ms;
  e.timeout_ms = 120000;
  std::string key = e.idempotency_key;
  ctx.pending.push_back(std::move(e));
  return json::dump(bj::object{{"accepted", true}, {"key", key}});
}

void Host::handle_pwa(const Request& req, const Finish& finish, const std::string& path, const std::string& tenant) {
  std::string request_id = req.header("goober-request-id");
  // /goober/pwa/$app/$file
  std::string rest = path.substr(std::string_view("/goober/pwa/").size());
  auto slash = rest.find('/');
  std::string name = rest.substr(0, slash), file = slash == std::string::npos ? "" : rest.substr(slash + 1);
  std::optional<site::App> app;
  if (site::is_app_name(name)) {
    for (auto& a : site::apps(*content_, tenant)) {
      if (a.name == name) app = std::move(a);
    }
  }
  if (!app) return finish(error_page(tenant, 404, request_id), R"({"pwa":"no such app"})");
  bool is_get = req.method == "get" || req.method == "head";
  bj::object extra{{"pwa", name}, {"file", file}};
  auto serve = [&](std::string body, std::string type, bool worker) {
    Response res;
    std::string etag = "\"" + base::sha256_hex(body).substr(0, 16) + "\"";
    res.headers.emplace_back("content-type", type);
    res.headers.emplace_back("cache-control", "no-cache");
    res.headers.emplace_back("etag", etag);
    res.headers.emplace_back("x-content-type-options", "nosniff");
    // The worker lives under /goober/pwa/, and controls the app's scope.
    if (worker) res.headers.emplace_back("service-worker-allowed", app->scope);
    if (req.header("if-none-match") == etag) {
      res.status = 304;
    } else {
      res.body = std::move(body);
    }
    res.head_only = req.method == "head";
    finish(std::move(res), json::dump(extra));
  };
  if (file == "manifest.webmanifest" && is_get) {
    return serve(json::dump(site::manifest_for(*app)), "application/manifest+json", false);
  }
  if (file == "sw.js" && is_get) {
    if (app->worker != "host") return finish(error_page(tenant, 404, request_id), R"({"pwa":"the app has its own worker"})");
    return serve(site::service_worker(*app, std::string(kBuildCommit)), "text/javascript; charset=utf-8", true);
  }
  std::string push_key;
  if (app->push) {
    std::string err;
    if (auto k = vapid_key(&err)) push_key = webpush::b64url(k->public_raw);
    else std::cerr << "goober-host: pwa: " << err << "\n";
  }
  if (file == "register.js" && is_get) return serve(site::register_script(*app, push_key), "text/javascript; charset=utf-8", false);
  if (file != "push") return finish(error_page(tenant, 404, request_id), R"({"pwa":"no such file"})");

  if (is_get) return finish(json_out(200, bj::object{{"push", app->push}, {"public_key", push_key}}), json::dump(extra));
  if (!app->push) return finish(json_out(403, bj::object{{"error", "this app doesn't take push"}}), json::dump(extra));
  // Same-origin JSON only: a cross-site form can't subscribe someone, or unsubscribe them.
  if (!base::to_lower(req.header("content-type")).starts_with("application/json") || !same_origin(req, origin_of(req))) {
    return finish(json_out(403, bj::object{{"error", "push subscriptions come as JSON from this site"}}), json::dump(extra));
  }
  std::string err;
  auto v = json::parse(req.body, &err);
  if (!v || !v->is_object()) return finish(json_out(400, bj::object{{"error", "the body is a JSON object"}}), json::dump(extra));
  const auto& o = v->as_object();
  auto db = open_push_db();
  int64_t now = base::now_unix_ms();
  if (req.method == "delete") {
    // Whoever holds an endpoint may drop it: the endpoint is the capability.
    sqlite::Stmt d(*db, "DELETE FROM subscriptions WHERE endpoint = ? AND tenant = ? AND app = ?");
    d.bind(1, text_of(o, "endpoint")).bind(2, tenant).bind(3, name);
    d.run();
    return finish(json_out(200, bj::object{{"removed", db->changes()}}), json::dump(extra));
  }
  if (req.method != "post") return finish(json_out(405, bj::object{{"error", "GET, POST, or DELETE"}}), json::dump(extra));
  auto who = caller(req, tenant);
  if (!who && !app->push_anonymous) {
    return finish(json_out(401, bj::object{{"error", "sign in to get notifications from this app"}}), json::dump(extra));
  }
  const auto* sub = o.if_contains("subscription");
  const bj::object* keys = sub && sub->is_object() ? (sub->as_object().if_contains("keys") && sub->as_object().at("keys").is_object()
                                                          ? &sub->as_object().at("keys").as_object()
                                                          : nullptr)
                                                   : nullptr;
  if (!sub || !sub->is_object() || !keys) {
    return finish(json_out(400, bj::object{{"error", "subscription is PushSubscription.toJSON(): {endpoint, keys: {p256dh, auth}}"}}),
                  json::dump(extra));
  }
  std::string endpoint = text_of(sub->as_object(), "endpoint"), p256dh = text_of(*keys, "p256dh"), auth = text_of(*keys, "auth");
  bool loopback = false;
  std::string why;
  if (!push_endpoint_allowed(endpoint, &loopback, &why)) return finish(json_out(400, bj::object{{"error", why}}), json::dump(extra));
  auto pk = webpush::b64url_decode(p256dh);
  auto ak = webpush::b64url_decode(auth);
  if (!pk || pk->size() != 65 || (*pk)[0] != '\x04' || !ak || ak->size() != 16) {
    return finish(json_out(400, bj::object{{"error", "keys.p256dh is a P-256 point and keys.auth 16 bytes, base64url"}}),
                  json::dump(extra));
  }
  bj::array topics;
  if (auto* t = o.if_contains("topics")) {
    if (!t->is_array() || t->as_array().size() > kTopicsMax) {
      return finish(json_out(400, bj::object{{"error", "topics is a list of at most 20 names"}}), json::dump(extra));
    }
    for (const auto& x : t->as_array()) {
      if (!x.is_string() || !is_topic(x.as_string())) {
        return finish(json_out(400, bj::object{{"error", "a topic is 1 to 64 of a-z, 0-9, _ and -"}}), json::dump(extra));
      }
      topics.push_back(x);
    }
  }
  if (auto replaces = text_of(o, "replaces"); !replaces.empty() && replaces != endpoint) {
    sqlite::Stmt d(*db, "DELETE FROM subscriptions WHERE endpoint = ? AND tenant = ? AND app = ?");
    d.bind(1, replaces).bind(2, tenant).bind(3, name);
    d.run();
  }
  db->exec("BEGIN IMMEDIATE");
  try {
    sqlite::Stmt up(*db,
                    "INSERT INTO subscriptions (tenant, app, user_id, endpoint, p256dh, auth, topics, created_ms) VALUES "
                    "(?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT (endpoint) DO UPDATE SET tenant = excluded.tenant, app = "
                    "excluded.app, user_id = excluded.user_id, p256dh = excluded.p256dh, auth = excluded.auth, topics = "
                    "excluded.topics, failures = 0");
    up.bind(1, tenant).bind(2, name);
    if (who) {
      up.bind(3, who->user.user_id);
    } else {
      up.bind_null(3);
    }
    up.bind(4, endpoint).bind(5, p256dh).bind(6, auth).bind(7, json::dump(topics)).bind(8, now);
    up.run();
    // A person's browsers: the newest twenty.
    if (who) {
      sqlite::Stmt cap(*db,
                       "DELETE FROM subscriptions WHERE tenant = ? AND user_id = ? AND id NOT IN (SELECT id FROM "
                       "subscriptions WHERE tenant = ? AND user_id = ? ORDER BY id DESC LIMIT ?)");
      cap.bind(1, tenant).bind(2, who->user.user_id).bind(3, tenant).bind(4, who->user.user_id).bind(5, int64_t(kSubscriptionsPerUser));
      cap.run();
    }
    db->exec("COMMIT");
  } catch (...) {
    db->exec("ROLLBACK");
    throw;
  }
  sqlite::Stmt id(*db, "SELECT id FROM subscriptions WHERE endpoint = ?");
  id.bind(1, endpoint);
  int64_t sub_id = id.step() ? id.int64(0) : 0;
  if (who) extra["user_id"] = std::to_string(who->user.user_id);
  finish(json_out(201, bj::object{{"subscribed", true}, {"id", sub_id}, {"topics", topics}}), json::dump(extra));
}

void Host::register_pwa_tools() {
  using mcp::object_property;
  using mcp::object_schema;
  using mcp::string_property;
  auto tenant_prop = string_property("A tenant name from the host config.");
  auto name_prop = string_property("The app's name: 1 to 64 of a-z, 0-9, _ and -.");

  mcp_->add_tool(
      {"pwa_put",
       "Makes part of a site, or all of it, an installable app (design/machine/782.PWA.md). app is {scope: \"/\" for the "
       "whole domain or \"/shop/\" for a part, manifest: {name, short_name, icons: [{src, sizes, type, purpose}], "
       "theme_color, background_color, display, start_url, shortcuts, ...}, worker: \"host\" (default: offline and push "
       "built in) | \"none\" | {src}, offline: {fallback: \"/shop/offline\", precache: [paths]}, push: true | false | "
       "{anonymous: true}}. The host serves the manifest and worker under /goober/pwa/$name/, and <!--#include pwa--> "
       "(already in the built-in base template) places the tags on every page in scope. The longest scope wins.",
       object_schema({{"tenant", tenant_prop}, {"name", name_prop}, {"app", object_property("The app's definition.")}},
                     {"tenant", "name", "app"}),
       [this](const bj::object& args) {
         std::string tenant = text_of(args, "tenant"), name = text_of(args, "name");
         auto* a = args.if_contains("app");
         if (tenant.empty() || name.empty() || !a || !a->is_object()) return ToolResult::fail("tenant, name, and app are required");
         std::string err;
         std::vector<std::string> warnings;
         std::string text = json::pretty(*a);
         auto app = site::parse_app(name, text, &err, &warnings);
         if (!app) return ToolResult::fail(err);
         for (const auto& other : site::apps(*content_, tenant)) {
           if (other.name != name && other.scope == app->scope) {
             return ToolResult::fail("app " + other.name + " already has scope " + app->scope);
           }
         }
         if (!content_->put(tenant, "_apps/" + name + ".json", text, &err)) return ToolResult::fail(err);
         bj::array w;
         for (auto& x : warnings) w.push_back(bj::value(x));
         std::string base = site::app_base(name);
         return ToolResult::ok(bj::object{{"name", name},
                                          {"scope", app->scope},
                                          {"manifest", base + "manifest.webmanifest"},
                                          {"worker", app->worker == "host" ? base + "sw.js" : app->worker_src},
                                          {"head", site::head_tags(*app)},
                                          {"warnings", std::move(w)}});
       }});

  mcp_->add_tool({"pwa_get", "One app: its definition, the manifest the host serves, and the tags pages in scope get.",
                  object_schema({{"tenant", tenant_prop}, {"name", name_prop}}, {"tenant", "name"}),
                  [this](const bj::object& args) {
                    std::string tenant = text_of(args, "tenant"), name = text_of(args, "name");
                    auto f = site::is_app_name(name) ? content_->get(tenant, "_apps/" + name + ".json") : std::nullopt;
                    if (!f) return ToolResult::fail("no app named " + name);
                    std::string err;
                    std::vector<std::string> warnings;
                    auto app = site::parse_app(name, f->data, &err, &warnings);
                    if (!app) return ToolResult::fail("_apps/" + name + ".json doesn't parse: " + err);
                    bj::array w;
                    for (auto& x : warnings) w.push_back(bj::value(x));
                    auto def = json::parse(f->data, &err);
                    return ToolResult::ok(bj::object{{"name", name},
                                                     {"app", def ? *def : bj::value()},
                                                     {"manifest", site::manifest_for(*app)},
                                                     {"head", site::head_tags(*app)},
                                                     {"warnings", std::move(w)}});
                  }});

  mcp_->add_tool({"pwa_list", "A tenant's apps, with each scope, its worker, whether it takes push, and its subscriptions.",
                  object_schema({{"tenant", tenant_prop}}, {"tenant"}), [this](const bj::object& args) {
                    std::string tenant = text_of(args, "tenant");
                    auto db = open_push_db();
                    bj::array out;
                    for (const auto& a : site::apps(*content_, tenant)) {
                      sqlite::Stmt c(*db, "SELECT COUNT(*) FROM subscriptions WHERE tenant = ? AND app = ?");
                      c.bind(1, tenant).bind(2, a.name);
                      out.push_back(bj::object{{"name", a.name},
                                               {"scope", a.scope},
                                               {"worker", a.worker},
                                               {"push", a.push},
                                               {"subscriptions", c.step() ? c.int64(0) : 0}});
                    }
                    return ToolResult::ok(bj::object{{"apps", std::move(out)}});
                  }});

  mcp_->add_tool({"pwa_delete",
                  "Removes an app: its pages stop naming a manifest. Installed copies keep their cached files until "
                  "the worker next updates; its subscriptions are kept until push services drop them.",
                  object_schema({{"tenant", tenant_prop}, {"name", name_prop}}, {"tenant", "name"}),
                  [this](const bj::object& args) {
                    std::string tenant = text_of(args, "tenant"), name = text_of(args, "name");
                    std::string err;
                    if (!site::is_app_name(name) || !content_->remove(tenant, "_apps/" + name + ".json", &err)) {
                      return ToolResult::fail(err.empty() ? "no app named " + name : err);
                    }
                    return ToolResult::ok(bj::object{{"deleted", name}});
                  }});

  mcp_->add_tool({"pwa_subscriptions",
                  "Who can be reached by push: counts per app, per topic, signed in or not, and how fresh. Endpoints "
                  "stay private: each is a capability to notify someone.",
                  object_schema({{"tenant", tenant_prop}, {"app", string_property("Only this app.")}}, {"tenant"}),
                  [this](const bj::object& args) {
                    std::string tenant = text_of(args, "tenant"), app = text_of(args, "app");
                    auto db = open_push_db();
                    sqlite::Stmt s(*db,
                                   "SELECT app, user_id, topics, last_ok_ms, failures, created_ms FROM subscriptions WHERE "
                                   "tenant = ? AND (? = '' OR app = ?)");
                    s.bind(1, tenant).bind(2, app).bind(3, app);
                    int64_t total = 0, signed_in = 0, failing = 0;
                    std::set<int64_t> people;
                    std::map<std::string, int64_t> by_app, by_topic;
                    std::string err;
                    while (s.step()) {
                      ++total;
                      ++by_app[s.text(0)];
                      if (!s.is_null(1)) {
                        ++signed_in;
                        people.insert(s.int64(1));
                      }
                      if (s.int64(4) > 0) ++failing;
                      if (auto t = json::parse(s.text(2), &err); t && t->is_array()) {
                        for (const auto& x : t->as_array()) {
                          if (x.is_string()) ++by_topic[std::string(x.as_string())];
                        }
                      }
                    }
                    bj::object apps, topics;
                    for (auto& [k, n] : by_app) apps[k] = n;
                    for (auto& [k, n] : by_topic) topics[k] = n;
                    return ToolResult::ok(bj::object{{"subscriptions", total},
                                                     {"signed_in", signed_in},
                                                     {"people", int64_t(people.size())},
                                                     {"anonymous", total - signed_in},
                                                     {"failing", failing},
                                                     {"by_app", std::move(apps)},
                                                     {"by_topic", std::move(topics)}});
                  }});

  mcp_->add_tool(
      {"push_send",
       "Sends a push now, to try an app's notifications: to is {user_id}, {topic}, {everyone: true}, or {subscription: "
       "id}; title, body, and url (a path on the site) are what the notification shows. Boxes send through a push service "
       "instead, after their request commits.",
       object_schema({{"tenant", tenant_prop},
                      {"app", string_property("Only this app's subscribers.")},
                      {"to", object_property("{user_id}, {topic}, {everyone: true}, or {subscription}.")},
                      {"title", string_property("The notification's title.")},
                      {"body", string_property("Its text.")},
                      {"url", string_property("Where a tap goes, like /shop/orders/7.")}},
                     {"tenant", "to"}),
       [this](const bj::object& args) {
         std::string tenant = text_of(args, "tenant");
         auto* to = args.if_contains("to");
         if (tenant.empty() || !to || !to->is_object()) return ToolResult::fail("tenant and to are required");
         std::string why;
         auto payload = push_payload(args, &why);
         if (!payload) return ToolResult::fail(why);
         const bj::object& target = to->as_object();
         bj::object report;
         auto r = deliver_push(tenant, text_of(args, "app"), target, *payload, {}, "", base::now_unix_ms(), &report);
         if (r.status == 400 || r.status == 500) return ToolResult::fail(r.error);
         return ToolResult::ok(std::move(report));
       }});
}

}  // namespace goober::host
