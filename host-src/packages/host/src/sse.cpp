// Subscriptions over server-sent events (design/human/040.TheBus.md, design/machine/036.Bus.md):
// a subscribe route is evaluated like a GET, read-only, whenever a topic it wakes on moves (at most
// once a coalescing window), or every refresh-ms anyway; subscribers with the same signature share
// one evaluation, and an answer identical to the last isn't sent.

#include <iostream>

#include "goober/base/hash.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/router/path.h"
#include "goober/types/types.h"
#include "http_util.h"

namespace goober::host {
namespace bj = boost::json;
using server::http::Request;
using server::http::Response;

namespace {

constexpr int64_t kKeepaliveMs = 15000;
constexpr int64_t kLifetimeMs = 15 * 60 * 1000;
// Each stream holds a connection from the proxy for its life: the host's total stays well under
// the server's 4096 connections, so ordinary requests always have room.
constexpr size_t kMaxStreams = 2000;
constexpr size_t kMaxStreamsPerTenant = 500;
constexpr size_t kMaxStreamsPerClient = 32;

// Scheduling runs on the monotonic clock: a wall clock stepped back by NTP must not stall
// keepalives, refreshes, or lifetimes.
int64_t mono_ms() { return base::monotonic_us() / 1000; }

// The answer as one SSE event: each line of the body a data: line.
std::string event(const std::string& name, int64_t id, const std::string& body) {
  std::string out = id >= 0 ? "id: " + std::to_string(id) + "\n" : "";
  out += "event: " + name + "\n";
  size_t start = 0;
  while (true) {
    size_t nl = body.find('\n', start);
    std::string line = body.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out += "data: " + line + "\n";
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return out + "\n";
}

}  // namespace

int64_t Host::base_seed() { return base::now_unix_ms() * 1000; }

Host::SseSlot::~SseSlot() {
  std::lock_guard lock(host->sse_count_mu_);
  --host->sse_total_;
  if (--host->sse_per_tenant_[tenant] == 0) host->sse_per_tenant_.erase(tenant);
  if (--host->sse_per_client_[client] == 0) host->sse_per_client_.erase(client);
}

std::optional<std::string> Host::resolve_topic(const std::string& tmpl, const std::string& tenant, const bj::object& doc,
                                               const std::optional<SignedIn>& who) {
  std::string out;
  size_t start = 0;
  while (true) {
    size_t slash = tmpl.find('/', start);
    std::string seg = tmpl.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg == "{!tenant}") {
      seg = tenant;
    } else if (seg == "{!user_id}") {
      if (!who) return std::nullopt;
      seg = std::to_string(who->user.user_id);
    } else if (seg == "{!domain}") {
      auto* d = doc.if_contains("domain");
      if (!d || !d->is_string()) return std::nullopt;
      seg = std::string(d->as_string());
    }
    out += (out.empty() ? "" : "/") + seg;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return out;
}

void Host::serve_subscription(const Request& req, const Finish& finish, boxes::RouteRef ref,
                              std::map<std::string, std::string> params, const std::string& domain, const std::string& tenant,
                              bool stream) {
  std::string request_id = req.header("goober-request-id");
  const auto& route = *ref.route;
  const auto& m = ref.box->manifest;
  std::optional<SignedIn> who;
  try {
    who = signed_in(req, tenant);
  } catch (const std::exception& e) {
    return finish(error_page(tenant, 500, request_id), R"({"why":"identity failed"})");
  }
  std::optional<manifest::Caller> caller;
  if (who) caller = manifest::Caller{std::to_string(who->user.user_id), who->host, who->roles};
  switch (manifest::check_access(route, caller ? &*caller : nullptr, params, tenant)) {
    case manifest::Access::kAllowed:
      break;
    case manifest::Access::kSignIn:
      return finish(error_page(tenant, 401, request_id), R"({"why":"sign in first"})");
    case manifest::Access::kForbidden:
      return finish(error_page(tenant, 403, request_id), R"({"why":"requirements not met"})");
    case manifest::Access::kBadValue:
      return finish(error_page(tenant, 400, request_id), R"({"why":"a substituted value holds / or :"})");
  }
  if (who && who->agent && !route.agents) {
    return finish(error_page(tenant, 403, request_id), R"({"why":"agent tokens can't call this route"})");
  }
  // What the route sees: its declared query, typed, or all of it; and only its declared headers
  // (never those hidden from boxes). Every header shown is part of the signature, so passing all
  // of them would give each browser (its user-agent, its languages) a group of its own.
  bj::object query;
  auto raw_query = router::parse_query(req.query);
  if (route.query_declared) {
    for (const auto& [name, schema] : route.query) {
      auto it = raw_query.find(name);
      if (it == raw_query.end()) {
        if (schema->default_value) query[name] = *schema->default_value;
        continue;
      }
      std::string err;
      auto v = types::check_text(*schema, it->second, &err);
      if (!v) return finish(error_page(tenant, 400, request_id), R"({"why":"bad query"})");
      query[name] = std::move(*v);
    }
  } else {
    for (const auto& [k, v] : raw_query) query[base::valid_utf8(k)] = base::valid_utf8(v);
  }
  bj::object headers;
  for (const auto& [k, v] : req.headers) {
    if (hidden_from_box(k)) continue;
    if (std::find(route.request_headers.begin(), route.request_headers.end(), k) == route.request_headers.end()) continue;
    headers[k] = v;
  }
  bj::object params_json;
  for (const auto& [k, v] : params) params_json[k] = v;
  bj::object doc{{"method", "subscribe"}, {"path", req.path},  {"params", params_json}, {"query", query},
                 {"headers", headers},    {"body", ""},        {"domain", domain},      {"tenant", tenant},
                 {"user", user_json(who)}};
  std::vector<std::string> topics;
  for (const auto& t : route.wake) {
    auto resolved = resolve_topic(t, tenant, doc, who);
    if (!resolved) return finish(error_page(tenant, 403, request_id), R"({"why":"a wake topic needs who is asking"})");
    topics.push_back(tenant + "/" + *resolved);
  }

  if (!stream) {
    // A plain GET: one evaluation, answered like any route's.
    doc["request_id"] = request_id;
    CallOptions o;
    o.read_only = true;
    auto call = call_box(ref, doc, tenant, request_id, who, o);
    Response res;
    if (call.result.outcome != runtime::wasm::CallResult::Outcome::kOk) {
      return finish(error_page(tenant, 500, request_id), json::dump(call.extra));
    }
    // The status vocabulary, exactly as for an HTTP route (037).
    if (std::string why = box_status_refusal(call.result.status, call.result.headers); !why.empty()) {
      log_line("boxes/" + m.box, bj::object{{"request_id", request_id}, {"version", ref.box->version}, {"level", "host"}, {"text", why}});
      return finish(error_page(tenant, 500, request_id), json::dump(call.extra));
    }
    res.status = call.result.status;
    if (route.response == "empty" && res.status >= 200 && res.status < 300) {
      res.status = 204;
      call.result.body.clear();
    }
    res.headers = std::move(call.result.headers);
    res.headers.emplace_back("content-type", call.result.content_type.empty() ? "application/octet-stream" : call.result.content_type);
    res.headers.emplace_back("x-content-type-options", "nosniff");
    res.headers.emplace_back("cache-control", "no-store");
    res.body = std::move(call.result.body);
    res.head_only = req.method == "head";
    return finish(std::move(res), json::dump(call.extra));
  }

  {
    std::lock_guard lock(sse_mu_);
    if (sse_stopping_) {
      auto res = error_page(tenant, 503, request_id);
      res.headers.emplace_back("retry-after", "5");
      return finish(std::move(res), R"({"why":"the host is stopping"})");
    }
  }
  // Limits (036 § 6): the host, the tenant, and one client address. The slot is taken here, so
  // a burst of requests can't all pass before any of them is counted.
  std::string client = rate_key(client_address(req));
  std::shared_ptr<SseSlot> slot;
  {
    std::lock_guard lock(sse_count_mu_);
    if (sse_total_ >= kMaxStreams || sse_per_tenant_[tenant] >= kMaxStreamsPerTenant ||
        sse_per_client_[client] >= kMaxStreamsPerClient) {
      if (sse_per_tenant_[tenant] == 0) sse_per_tenant_.erase(tenant);
      if (sse_per_client_[client] == 0) sse_per_client_.erase(client);
      auto res = error_page(tenant, 429, request_id);
      res.headers.emplace_back("retry-after", "30");
      return finish(std::move(res), R"({"why":"too many open subscriptions"})");
    }
    ++sse_total_, ++sse_per_tenant_[tenant], ++sse_per_client_[client];
    slot = std::shared_ptr<SseSlot>(new SseSlot{this, tenant, client});
  }
  // The signature (036 § 5): everything the box is shown, so the same question is answered
  // once: the domain, the route and its inputs, and who asks (a person's agent apart from them).
  std::string roles;
  if (who) {
    for (const auto& r : who->roles) roles += r + ",";
  }
  std::string signature = tenant + "\n" + domain + "\n" + m.box + "\n" + route.name + "\n" + json::dump(params_json) + "\n" +
                          json::dump(query) + "\n" + json::dump(headers) + "\n" +
                          (who ? std::to_string(who->user.user_id) + "\n" + roles + "\n" + (who->agent ? "agent:" : "person:") +
                                     who->client_id + "\n" + (who->host ? "host" : "")
                               : "-");
  std::string key = base::sha256_hex(signature);
  int64_t last_seen = -1;
  if (auto lei = req.header("last-event-id"); !lei.empty() && lei.size() < 20 &&
                                               std::all_of(lei.begin(), lei.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    last_seen = std::stoll(lei);
  }
  Response res;
  res.status = 200;
  res.headers = {{"content-type", "text/event-stream; charset=utf-8"},
                 {"cache-control", "no-store"},
                 {"x-accel-buffering", "no"},
                 {"x-content-type-options", "nosniff"}};
  std::string box = m.box, route_name = route.name;
  int64_t coalesce = route.coalesce_ms, refresh = route.refresh_ms;
  res.stream = [this, key, tenant, domain, path = req.path, box, route_name, who, doc, topics, coalesce, refresh, slot,
                last_seen](std::shared_ptr<server::http::Stream> s) {
    auto sub = std::make_shared<SseSub>();
    sub->stream = s;
    sub->slot = slot;
    // Lifetimes spread over the last tenth, so streams opened together don't reconnect together.
    sub->expires_ms = mono_ms() + kLifetimeMs - int64_t(base::os_random_u64() % uint64_t(kLifetimeMs / 10));
    sub->last_sent = last_seen;
    std::shared_ptr<SseGroup> g;
    std::string first;
    {
      std::lock_guard lock(sse_mu_);
      if (sse_stopping_) {
        sub.reset();
      }
    }
    if (!sub) return s->close();  // the host began stopping after this request was admitted
    {
      std::lock_guard lock(sse_mu_);
      auto& slot = sse_groups_[key];
      if (!slot) {
        slot = std::make_shared<SseGroup>();
        slot->key = key;
        slot->tenant = tenant;
        slot->domain = domain;
        slot->path = path;
        slot->box = box;
        slot->route = route_name;
        slot->who = who;
        slot->doc = doc;
        slot->topics = topics;
        slot->coalesce_ms = coalesce;
        slot->refresh_ms = refresh;
        std::weak_ptr<SseGroup> weak = slot;
        slot->watch_id = bus_.watch(topics, [this, weak](const std::string&, int64_t) {
          std::lock_guard l2(sse_mu_);
          if (auto grp = weak.lock()) {
            int64_t at = std::max(mono_ms(), grp->last_eval_ms + grp->coalesce_ms);
            if (!grp->due_ms || at < grp->due_ms) grp->due_ms = at;
          }
          sse_cv_.notify_all();
        });
      }
      g = slot;
      g->subs.push_back(sub);
      // Its first answer: the cached one when it's current, and new to this subscriber; else an
      // evaluation now.
      int64_t current = bus_.max_seq(g->topics);
      if (g->last_id >= 0 && g->last_id >= current && !g->last_body.empty()) {
        if (sub->last_sent != g->last_id) first = event("update", g->last_id, g->last_body);
        sub->last_sent = g->last_id;
        sub->has_answer = true;
      } else if (!g->due_ms || g->due_ms > mono_ms()) {
        g->due_ms = mono_ms();
      }
    }
    if (!first.empty()) s->send(first);
    std::weak_ptr<SseSub> weak_sub = sub;
    s->on_close([this, key, weak_sub] {
      std::shared_ptr<SseSub> gone;  // let go after the lock: its slot takes the count lock
      std::lock_guard lock(sse_mu_);
      auto it = sse_groups_.find(key);
      if (it == sse_groups_.end()) return;
      auto& subs = it->second->subs;
      auto me = weak_sub.lock();
      if (auto at = std::find(subs.begin(), subs.end(), me); at != subs.end()) {
        gone = std::move(*at);
        *at = std::move(subs.back());
        subs.pop_back();
      }
      if (subs.empty()) {
        bus_.unwatch(it->second->watch_id);
        sse_groups_.erase(it);
      }
    });
    sse_cv_.notify_all();
  };
  finish(std::move(res), json::dump(bj::object{{"sse", "open"}, {"box", box}, {"route", route_name}}));
}

void Host::sse_evaluate(std::shared_ptr<SseGroup> g) {
  int64_t started = mono_ms();
  std::string message;
  bool end = false, changed = false, send_body = false;
  std::string body;
  int64_t id = -1;
  // Whatever throws, the group is let go (running cleared) and its subscribers told, so one
  // bad evaluation never freezes a group.
  try {
    id = bus_.max_seq(g->topics);
    // The route as served now: a redeploy that dropped it ends the streams.
    auto active = boxes_->active();
    const auto& table = active->table_for(g->domain);
    auto path = router::normalize_path(g->path);
    std::optional<boxes::RouteRef> ref;
    if (path) {
      auto match = table.match(path->segments, "subscribe");
      if (match.value && match.value->box->manifest.box == g->box && match.value->route->name == g->route) ref = *match.value;
    }
    if (!ref) {
      message = event("error", -1, R"({"status":404,"error":"the subscription is no longer served"})");
      end = true;
    } else {
      bj::object doc = g->doc;
      std::string request_id = "sse-" + g->key.substr(0, 12) + "-" + std::to_string(base::now_unix_ms());
      doc["request_id"] = request_id;
      CallOptions o;
      o.read_only = true;
      auto call = call_box(*ref, doc, g->tenant, request_id, g->who, o);
      const auto& r = call.result;
      if (r.outcome != runtime::wasm::CallResult::Outcome::kOk || r.status >= 500) {
        message = event("error", -1, R"({"status":500})");
      } else if (r.status < 200 || r.status >= 300) {
        message = event("error", -1, json::dump(bj::object{{"status", r.status}}));
        end = r.status == 401 || r.status == 403;
      } else {
        body = r.body;
        send_body = true;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "goober-host: subscription " << g->box << "." << g->route << ": " << e.what() << "\n";
    message = event("error", -1, R"({"status":500})");
    send_body = false;
  }
  // Who gets what is decided under the lock; sending and closing happen outside it, since a
  // stream that closes calls back into the hub.
  std::vector<std::shared_ptr<server::http::Stream>> send_to, all;
  {
    std::lock_guard lock(sse_mu_);
    ++g->evaluations;
    g->last_eval_ms = mono_ms();
    g->last_eval_at = base::now_unix_ms();
    g->running = false;
    if (send_body) {
      // Identical to the last: nothing is sent (040), but to a subscriber who has no answer yet.
      changed = body != g->last_body || g->last_id < 0;
      g->last_body = std::move(body);
      g->last_id = id;
      message = event("update", id, g->last_body);
    }
    // A wake during the evaluation leaves it due again; one before it was served by it.
    if (g->due_ms && g->due_ms <= started) g->due_ms = 0;
    for (const auto& s : g->subs) {
      all.push_back(s->stream);
      if (!send_body) {
        if (!message.empty()) send_to.push_back(s->stream);
      } else if (changed || !s->has_answer) {
        send_to.push_back(s->stream);
        s->has_answer = true;
        s->last_sent = id;
      }
    }
  }
  // One string for every subscriber.
  auto shared = std::make_shared<const std::string>(std::move(message));
  for (const auto& s : send_to) s->send(shared);
  if (end) {
    for (const auto& s : all) s->close();
  }
}

void Host::sse_loop() {
  int64_t last_ping = mono_ms();
  auto ping = std::make_shared<const std::string>(": ping\n\n");
  std::unique_lock lock(sse_mu_);
  while (!sse_stopping_) {
    sse_cv_.wait_for(lock, std::chrono::milliseconds(100));
    if (sse_stopping_) break;
    int64_t now = mono_ms();
    bool ping_now = now - last_ping >= kKeepaliveMs;
    std::vector<std::shared_ptr<SseGroup>> due;
    std::vector<std::shared_ptr<server::http::Stream>> pings, expire;
    for (auto& [_, g] : sse_groups_) {
      bool wake = g->due_ms && g->due_ms <= now;
      bool refresh = g->last_eval_ms && now - g->last_eval_ms >= g->refresh_ms;
      if (!g->running && (wake || refresh)) {
        g->running = true;
        due.push_back(g);
      }
      for (const auto& s : g->subs) {
        if (now >= s->expires_ms) expire.push_back(s->stream);
        else if (ping_now) pings.push_back(s->stream);
      }
    }
    if (ping_now) last_ping = now;
    lock.unlock();
    for (auto& g : due) {
      bool queued = false;
      try {
        queued = pool_->post([this, g] { sse_evaluate(g); }, max_queued_);
      } catch (const std::exception&) {
      }
      if (!queued) {
        std::lock_guard l2(sse_mu_);
        g->running = false;  // the pool is full: tried again next round
      }
    }
    for (auto& s : pings) s->send(ping);
    // A stream's life is about 15 minutes: the browser reconnects, and the request is checked again.
    for (auto& s : expire) s->close();
    lock.lock();
  }
}

void Host::sse_stop() {
  std::vector<std::shared_ptr<server::http::Stream>> all;
  {
    std::lock_guard lock(sse_mu_);
    sse_stopping_ = true;
    for (auto& [_, g] : sse_groups_) {
      for (auto& s : g->subs) all.push_back(s->stream);
    }
  }
  sse_cv_.notify_all();
  if (sse_thread_.joinable()) sse_thread_.join();
  for (auto& s : all) s->close();
}

void Host::sse_clear() {
  // After the server stopped: whatever is left goes before the server it belongs to.
  std::vector<std::shared_ptr<server::http::Stream>> all;
  std::map<std::string, std::shared_ptr<SseGroup>> groups;
  {
    std::lock_guard lock(sse_mu_);
    for (auto& [_, g] : sse_groups_) {
      bus_.unwatch(g->watch_id);
      for (auto& s : g->subs) all.push_back(s->stream);
    }
    groups.swap(sse_groups_);
  }
  for (auto& s : all) s->close();
}

bj::object Host::bus_status(const std::string& tenant) {
  bj::array topics;
  for (const auto& t : bus_.topics(tenant + "/", 200)) {
    topics.push_back(bj::object{{"topic", t.name.substr(tenant.size() + 1)},
                                {"seq", t.seq},
                                {"published", t.at_ms ? bj::value(base::format_utc_ms(t.at_ms)) : bj::value(nullptr)},
                                {"publishes", t.publishes},
                                {"watchers", int64_t(t.watchers)}});
  }
  bj::array groups;
  std::lock_guard lock(sse_mu_);
  for (const auto& [_, g] : sse_groups_) {
    if (g->tenant != tenant) continue;
    groups.push_back(bj::object{{"box", g->box},
                                {"route", g->route},
                                {"path", g->path},
                                {"subscribers", int64_t(g->subs.size())},
                                {"evaluations", g->evaluations},
                                {"last_evaluation", g->last_eval_at ? bj::value(base::format_utc_ms(g->last_eval_at)) : bj::value(nullptr)},
                                {"last_bytes", int64_t(g->last_body.size())},
                                {"signed_in", g->who.has_value()}});
  }
  return bj::object{{"tenant", tenant}, {"topics", std::move(topics)}, {"subscriptions", std::move(groups)}};
}

}  // namespace goober::host
