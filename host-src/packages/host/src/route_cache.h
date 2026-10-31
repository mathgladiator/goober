#pragma once

// Route caching (design/machine/037.Caching.md § 4), private to the host package: the box's
// answers to GET routes that declare "cache", kept in memory under the host's budget
// (cache.bytes, at most a quarter per tenant), least recently used out first, by bytes.
// An entry is the box's own answer (before templates and partials, which the host renders
// for each request), keyed by the route's signature, with what it depended on: the version of
// each database its box can read (037 § 3), or the sequences of its wake topics, and the
// flags it read. The host decides whether an entry is still good; this class only keeps them
// and counts what happened, per route.

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "goober/host/host.h"
#include "goober/json/json.h"

namespace goober::host {

struct CacheEntry {
  std::string tenant, box, version, route;
  // The box's answer.
  int status = 200;
  std::string content_type;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  // What it depended on (by "data": databases; by "wake": topics), and the flags it read.
  struct Db {
    std::string file;
    int64_t version = 0;
    std::optional<int64_t> data_version;
  };
  std::vector<Db> dbs;
  std::vector<std::pair<std::string, int64_t>> topics;  // bus topic (tenant/...), sequence
  std::vector<std::pair<std::string, std::string>> flags;  // name, value read
  int64_t stored_ms = 0, fresh_until_ms = 0;
  // For the sampled checks (037 § 4.3): the input the box was called with, and as whom.
  boost::json::object doc;
  std::optional<SignedIn> who;
  size_t bytes() const;
  // Whether another answer is the same: status, content type, headers, and bytes.
  bool same_answer(const CacheEntry& o) const;
};

// What happened to a route's requests, since the host started. The perf report (037 § 7)
// reads these; the access log's "cache" field says the same per request.
struct RouteCacheStats {
  int64_t hits = 0, misses = 0, stale = 0, not_modified = 0, stored = 0, checks = 0, mismatches = 0,
          evictions = 0;
};

class RouteCache {
 public:
  // `budget` is read on each store: the host's cache.bytes, which a reload may change.
  explicit RouteCache(std::function<int64_t()> budget) : budget_(std::move(budget)) {}

  std::optional<CacheEntry> get(const std::string& key);
  void put(const std::string& key, CacheEntry entry);
  void erase(const std::string& key);
  // Entries of a tenant (and one route of it, when given); how many went.
  int clear(const std::string& tenant, const std::string& route = "");

  // "hit", "miss", "stale", "304", "stored", "check", "mismatch".
  void count(const std::string& tenant, const std::string& box, const std::string& route, const std::string& what);
  // By "box/route", for a tenant.
  std::map<std::string, RouteCacheStats> stats(const std::string& tenant);
  // {entries, bytes, budget, tenant_budget}
  boost::json::object usage(const std::string& tenant);

  // A sampled check runs one at a time per key.
  bool begin_check(const std::string& key);
  void end_check(const std::string& key);

 private:
  struct Slot {
    CacheEntry entry;
    size_t bytes = 0;
    std::list<std::string>::iterator order;
  };
  void evict_until(const std::string& tenant, int64_t tenant_budget, int64_t budget);  // with mu_ held
  void drop(std::map<std::string, Slot>::iterator it, bool evicted);  // with mu_ held

  std::function<int64_t()> budget_;
  std::mutex mu_;
  std::map<std::string, Slot> slots_;
  std::list<std::string> order_;  // least recently used first
  std::map<std::string, int64_t> tenant_bytes_;
  int64_t total_bytes_ = 0;
  std::map<std::string, std::map<std::string, RouteCacheStats>> stats_;  // tenant -> box/route
  std::set<std::string> checking_;
};

}  // namespace goober::host
