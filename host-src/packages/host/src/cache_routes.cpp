// Route caching in the host (design/machine/037.Caching.md § 4 and § 5): the signature, what an
// entry depends on, when it goes stale, storing, the sampled checks, and the operator tools.

#include <iostream>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "route_cache.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

bool has_header(const std::vector<std::pair<std::string, std::string>>& headers, std::string_view name) {
  for (const auto& [k, v] : headers) {
    if (base::to_lower(k) == name) return true;
  }
  return false;
}

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

}  // namespace

Host::CacheLookup Host::cache_lookup(const boxes::RouteRef& ref, bj::object& doc, const std::string& tenant,
                                     const std::optional<SignedIn>& who, const std::string& method) {
  CacheLookup out;
  const auto& route = *ref.route;
  const auto& m = ref.box->manifest;
  if (!route.cache.on || (method != "get" && method != "head") || !route_cache_ || config_.get()->cache_bytes <= 0) {
    return out;
  }
  out.on = true;
  // The box of a cached route sees only what the signature names (037 § 4.1): a GET and a
  // HEAD alike, the request headers it declares and no others, no client key, and for a public
  // route, no one: one entry serves everyone, so it can't depend on who asked.
  doc["method"] = "get";
  doc.erase("client_key");
  if (!route.headers_declared) doc["headers"] = bj::object{};
  if (route.is_public) doc["user"] = nullptr;
  bj::object signature = doc;
  signature.erase("request_id");
  out.key = tenant + "\n" + m.box + "\n" + ref.box->version + "\n" + route.name + "\n" +
            base::sha256_hex(json::dump(signature));

  // What the answer will depend on, read before the box runs: a commit landing in between
  // makes the recorded version older than what the box saw, which only invalidates sooner.
  out.pending = std::make_shared<CacheEntry>();
  out.pending->tenant = tenant;
  out.pending->box = m.box;
  out.pending->version = ref.box->version;
  out.pending->route = route.name;
  out.pending->doc = doc;
  out.pending->who = route.is_public ? std::nullopt : who;
  if (route.cache.by == "data") {
    for (const auto& d : m.data) {
      if (!d.applies_schema()) continue;
      auto file = databases_->file(d.tenant_for(tenant), d.database_for(tenant));
      out.pending->dbs.push_back({file.string(), databases_->version(file), databases_->data_version(file)});
    }
  } else {
    for (const auto& t : route.cache.wake) {
      auto topic = resolve_topic(t, tenant, doc, out.pending->who);
      if (!topic) {
        out.on = false;  // a topic this caller can't name: answer without the cache
        return out;
      }
      std::string full = tenant + "/" + *topic;
      out.pending->topics.emplace_back(full, bus_.seq(full));
    }
  }

  auto entry = route_cache_->get(out.key);
  if (!entry) {
    out.outcome = "miss";
    route_cache_->count(tenant, m.box, route.name, "miss");
    return out;
  }
  if (auto why = cache_stale(*entry, m, tenant)) {
    route_cache_->erase(out.key);
    out.outcome = "stale";
    route_cache_->count(tenant, m.box, route.name, "stale");
    return out;
  }
  out.outcome = "hit";
  route_cache_->count(tenant, m.box, route.name, "hit");
  BoxCall hit;
  hit.result.outcome = runtime::wasm::CallResult::Outcome::kOk;
  hit.result.status = entry->status;
  hit.result.content_type = entry->content_type;
  hit.result.headers = entry->headers;
  hit.result.body = entry->body;
  hit.extra = bj::object{{"box", m.box},
                         {"version", ref.box->version},
                         {"route", route.name},
                         {"outcome", "ok"},
                         {"now_ms", base::now_unix_ms()}};
  out.hit = std::move(hit);
  // A share of hits also run the box, off the request's path, to check the promise (§ 4.3).
  if (route.cache.check_percent > 0 && int64_t(base::os_random_u64() % 100) < route.cache.check_percent) {
    cache_check(ref, out.key, std::move(*entry));
  }
  return out;
}

std::optional<std::string> Host::cache_stale(const CacheEntry& e, const manifest::Manifest& m, const std::string& tenant) {
  if (base::now_unix_ms() >= e.fresh_until_ms) return "fresh-ms passed";
  for (const auto& d : e.dbs) {
    if (databases_->version(d.file) != d.version) return "a database it read changed";
    if (databases_->data_version(d.file) != d.data_version) return "a database it read changed outside the host";
  }
  for (const auto& [topic, seq] : e.topics) {
    if (bus_.seq(topic) != seq) return "a topic it wakes on moved";
  }
  for (const auto& [name, value] : e.flags) {
    if (flag_for(m, tenant, name).value_or("") != value) return "a flag it read changed";
  }
  return std::nullopt;
}

std::optional<CacheEntry> Host::cache_entry(const CacheLookup& lookup, const manifest::HttpRoute& route,
                                            const BoxCall& call) {
  const auto& r = call.result;
  // Stored (§ 4.2): a 200 or 204 the box gave without calling out, without a cookie, up to 1 MiB.
  if (r.outcome != runtime::wasm::CallResult::Outcome::kOk || (r.status != 200 && r.status != 204)) return std::nullopt;
  if (call.calls_out > 0 || has_header(r.headers, "set-cookie") || r.body.size() > (1u << 20)) return std::nullopt;
  CacheEntry e = *lookup.pending;
  e.status = r.status;
  e.content_type = r.content_type;
  e.headers = r.headers;
  e.body = r.body;
  if (const auto* f = call.extra.if_contains("flags"); f && f->is_object()) {
    for (const auto& kv : f->as_object()) {
      e.flags.emplace_back(std::string(kv.key()), kv.value().is_string() ? std::string(kv.value().as_string()) : "");
    }
  }
  e.stored_ms = base::now_unix_ms();
  e.fresh_until_ms = e.stored_ms + route.cache.fresh_ms;
  return e;
}

void Host::cache_store(const CacheLookup& lookup, const manifest::HttpRoute& route, const BoxCall& call) {
  if (!lookup.on || lookup.hit) return;
  if (auto e = cache_entry(lookup, route, call)) route_cache_->put(lookup.key, std::move(*e));
}

void Host::cache_check(const boxes::RouteRef& ref, const std::string& key, CacheEntry entry) {
  if (!route_cache_->begin_check(key)) return;
  pool_->post([this, ref, key, entry = std::move(entry)]() mutable {
    struct Done {
      RouteCache& cache;
      std::string key;
      ~Done() { cache.end_check(key); }
    } done{*route_cache_, key};
    try {
      const auto& route = *ref.route;
      // Fresh dependencies, read before the box runs again, as a miss would.
      CacheLookup again;
      again.on = true;
      again.key = key;
      again.pending = std::make_shared<CacheEntry>(entry);
      again.pending->status = 200;
      again.pending->headers.clear();
      again.pending->body.clear();
      again.pending->flags.clear();
      for (auto& d : again.pending->dbs) {
        d.version = databases_->version(d.file);
        d.data_version = databases_->data_version(d.file);
      }
      for (auto& [topic, seq] : again.pending->topics) seq = bus_.seq(topic);
      uint8_t rnd[8];
      base::os_random_fill(rnd);
      std::string request_id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
      bj::object doc = entry.doc;
      doc["request_id"] = request_id;
      CallOptions options;
      options.read_only = true;
      auto call = call_box(ref, doc, entry.tenant, request_id, entry.who, options);
      route_cache_->count(entry.tenant, entry.box, entry.route, "check");
      auto fresh = cache_entry(again, route, call);
      if (fresh && fresh->same_answer(entry)) {
        route_cache_->put(key, std::move(*fresh));
        return;
      }
      // The promise didn't hold: the answer depends on more than the route declares (the
      // clock, randomness, the request id, a table its wake list misses). Counted, logged,
      // and the entry replaced with what the box answers now (or dropped).
      route_cache_->count(entry.tenant, entry.box, entry.route, "mismatch");
      perf_->record_cache(entry.tenant, entry.box, entry.route, "mismatch", base::now_unix_ms());
      log_line("boxes/" + entry.box,
               bj::object{{"at", base::format_utc_ms(base::now_unix_ms())},
                          {"request_id", request_id},
                          {"version", entry.version},
                          {"level", "host"},
                          {"text", "cache check: route " + entry.route + " answered differently from its cached answer "
                                   "(signature " + key.substr(key.rfind('\n') + 1, 16) + "); the answer depends on more "
                                   "than its cache declaration says (037 § 4.3)"}});
      if (fresh) {
        route_cache_->put(key, std::move(*fresh));
      } else {
        route_cache_->erase(key);
      }
    } catch (const std::exception& e) {
      std::cerr << "goober-host: cache check: " << e.what() << "\n";
      route_cache_->erase(key);
    }
  });
}

void Host::cache_headers(const manifest::HttpRoute& route, const std::optional<SignedIn>& who,
                         server::http::Response& res) {
  // What browsers and proxies may keep (§ 4): the host's word, not the box's. Shared only for a
  // public route asked by no one; a signed-in page is that person's.
  std::erase_if(res.headers, [](const auto& h) { return h.first == "cache-control"; });
  std::string scope = route.is_public && !who ? "public" : "private";
  if (route.cache.browser_ms == 0) {
    res.headers.emplace_back("cache-control", scope + ", no-cache");
  } else {
    res.headers.emplace_back("cache-control", scope + ", max-age=" + std::to_string(route.cache.browser_ms / 1000));
  }
}

void Host::register_cache_tools() {
  route_cache_ = std::make_unique<RouteCache>([this] { return config_.get()->cache_bytes; });
  auto tenant_prop = mcp::string_property("A tenant name, like example.");
  mcp_->add_tool(
      {"cache_status",
       "Read tool. The route cache of a tenant (design/machine/037.Caching.md): its entries and bytes against the "
       "budget (host.json cache.bytes, a quarter per tenant), and for each box route that declares \"cache\", since "
       "the host started: hits (answered without waking the box), misses, stale entries (a database it read "
       "changed, a wake topic moved, fresh-ms passed, or a flag changed), 304s, entries stored, sampled checks, "
       "mismatches (the box answered differently: its answer depends on more than its declaration says), and "
       "evictions. Also each cached route's declaration.",
       mcp::object_schema({{"tenant", tenant_prop}}, {"tenant"}), [this](const bj::object& args) {
         auto tenant = arg(args, "tenant").value_or("");
         auto stats = route_cache_->stats(tenant);
         bj::array routes;
         for (const auto& [name, box] : boxes_->active()->boxes) {
           for (const auto& r : box->manifest.routes) {
             if (!r.cache.on) continue;
             RouteCacheStats s;
             if (auto it = stats.find(name + "/" + r.name); it != stats.end()) s = it->second;
             bj::array wake;
             for (const auto& w : r.cache.wake) wake.push_back(bj::value(w));
             routes.push_back(bj::object{{"box", name},
                                         {"route", r.name},
                                         {"path", r.pattern.source},
                                         {"by", r.cache.by},
                                         {"wake", std::move(wake)},
                                         {"fresh_ms", r.cache.fresh_ms},
                                         {"browser_ms", r.cache.browser_ms},
                                         {"check_percent", r.cache.check_percent},
                                         {"hits", s.hits},
                                         {"misses", s.misses},
                                         {"stale", s.stale},
                                         {"not_modified", s.not_modified},
                                         {"stored", s.stored},
                                         {"checks", s.checks},
                                         {"mismatches", s.mismatches},
                                         {"evictions", s.evictions}});
           }
         }
         bj::object out = route_cache_->usage(tenant);
         out["tenant"] = tenant;
         out["routes"] = std::move(routes);
         return ToolResult::ok(std::move(out));
       }});
  mcp_->add_tool(
      {"cache_clear",
       "Changing tool: drops a tenant's cached route answers, all of them or one route's (by route name), so the "
       "next request runs the box. Waits for approval when an agent's approvals are on. Nothing else changes: "
       "entries also go stale on their own when what they read changes. Answers how many went.",
       mcp::object_schema({{"tenant", tenant_prop},
                           {"route", mcp::string_property("A route name, like menu; omit for all of the tenant's.")}},
                          {"tenant"}),
       [this](const bj::object& args) {
         auto tenant = arg(args, "tenant").value_or("");
         if (tenant.empty()) return ToolResult::fail("tenant is required");
         int n = route_cache_->clear(tenant, arg(args, "route").value_or(""));
         return ToolResult::ok(bj::object{{"tenant", tenant}, {"cleared", n}});
       }});
}

}  // namespace goober::host
