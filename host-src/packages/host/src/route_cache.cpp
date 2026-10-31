// Route caching (design/machine/037.Caching.md § 4): the entries and their counters.

#include "route_cache.h"

namespace goober::host {
namespace bj = boost::json;

size_t CacheEntry::bytes() const {
  size_t n = body.size() + content_type.size() + 256;
  for (const auto& [k, v] : headers) n += k.size() + v.size() + 16;
  n += json::dump(doc).size();
  for (const auto& d : dbs) n += d.file.size() + 32;
  for (const auto& [t, s] : topics) n += t.size() + 16;
  for (const auto& [k, v] : flags) n += k.size() + v.size() + 16;
  return n;
}

bool CacheEntry::same_answer(const CacheEntry& o) const {
  return status == o.status && content_type == o.content_type && headers == o.headers && body == o.body;
}

std::optional<CacheEntry> RouteCache::get(const std::string& key) {
  std::lock_guard lock(mu_);
  auto it = slots_.find(key);
  if (it == slots_.end()) return std::nullopt;
  order_.splice(order_.end(), order_, it->second.order);  // most recently used
  return it->second.entry;
}

void RouteCache::drop(std::map<std::string, Slot>::iterator it, bool evicted) {
  const auto& e = it->second.entry;
  if (evicted) ++stats_[e.tenant][e.box + "/" + e.route].evictions;
  tenant_bytes_[e.tenant] -= int64_t(it->second.bytes);
  total_bytes_ -= int64_t(it->second.bytes);
  order_.erase(it->second.order);
  slots_.erase(it);
}

void RouteCache::evict_until(const std::string& tenant, int64_t tenant_budget, int64_t budget) {
  // The tenant's own least recently used first, until it fits its quarter; then anyone's.
  for (auto o = order_.begin(); o != order_.end() && tenant_bytes_[tenant] > tenant_budget;) {
    auto it = slots_.find(*o);
    ++o;
    if (it != slots_.end() && it->second.entry.tenant == tenant) drop(it, true);
  }
  while (total_bytes_ > budget && !order_.empty()) {
    auto it = slots_.find(order_.front());
    if (it == slots_.end()) {
      order_.pop_front();
      continue;
    }
    drop(it, true);
  }
}

void RouteCache::put(const std::string& key, CacheEntry entry) {
  int64_t budget = budget_ ? budget_() : 0;
  int64_t tenant_budget = budget / 4;
  size_t bytes = entry.bytes();
  std::lock_guard lock(mu_);
  if (auto it = slots_.find(key); it != slots_.end()) drop(it, false);
  if (budget <= 0 || int64_t(bytes) > tenant_budget) return;  // caching off, or larger than the tenant may hold
  std::string tenant = entry.tenant;
  ++stats_[tenant][entry.box + "/" + entry.route].stored;
  order_.push_back(key);
  Slot slot{std::move(entry), bytes, std::prev(order_.end())};
  slots_.emplace(key, std::move(slot));
  tenant_bytes_[tenant] += int64_t(bytes);
  total_bytes_ += int64_t(bytes);
  evict_until(tenant, tenant_budget, budget);
}

void RouteCache::erase(const std::string& key) {
  std::lock_guard lock(mu_);
  if (auto it = slots_.find(key); it != slots_.end()) drop(it, false);
}

int RouteCache::clear(const std::string& tenant, const std::string& route) {
  std::lock_guard lock(mu_);
  int n = 0;
  for (auto it = slots_.begin(); it != slots_.end();) {
    auto next = std::next(it);
    if (it->second.entry.tenant == tenant && (route.empty() || it->second.entry.route == route)) {
      drop(it, false);
      ++n;
    }
    it = next;
  }
  return n;
}

void RouteCache::count(const std::string& tenant, const std::string& box, const std::string& route,
                       const std::string& what) {
  std::lock_guard lock(mu_);
  auto& s = stats_[tenant][box + "/" + route];
  if (what == "hit") ++s.hits;
  else if (what == "miss") ++s.misses;
  else if (what == "stale") ++s.stale;
  else if (what == "304") ++s.not_modified;
  else if (what == "check") ++s.checks;
  else if (what == "mismatch") ++s.mismatches;
}

std::map<std::string, RouteCacheStats> RouteCache::stats(const std::string& tenant) {
  std::lock_guard lock(mu_);
  auto it = stats_.find(tenant);
  return it == stats_.end() ? std::map<std::string, RouteCacheStats>{} : it->second;
}

bj::object RouteCache::usage(const std::string& tenant) {
  int64_t budget = budget_ ? budget_() : 0;
  std::lock_guard lock(mu_);
  int64_t entries = 0;
  for (const auto& [k, s] : slots_) entries += s.entry.tenant == tenant;
  auto tb = tenant_bytes_.find(tenant);
  return bj::object{{"entries", entries},
                    {"bytes", tb == tenant_bytes_.end() ? int64_t(0) : tb->second},
                    {"tenant_budget", budget / 4},
                    {"host_bytes", total_bytes_},
                    {"host_budget", budget}};
}

bool RouteCache::begin_check(const std::string& key) {
  std::lock_guard lock(mu_);
  return checking_.insert(key).second;
}

void RouteCache::end_check(const std::string& key) {
  std::lock_guard lock(mu_);
  checking_.erase(key);
}

}  // namespace goober::host
