#include "goober/operations/live.h"

#include <algorithm>

#include "goober/operations/traffic.h"

namespace goober::operations {

LiveStats::LiveStats(size_t max_paths, size_t max_flows) : max_paths_(max_paths), max_flows_(max_flows) {}

void LiveStats::record(int64_t now_ms, const std::string& tenant, const std::string& path_in, const std::string& route,
                       const std::string& box, int status, int64_t us, const std::string& from) {
  std::string path = path_in.size() > 200 ? path_in.substr(0, 200) : path_in;
  int64_t at = now_ms / kBucketMs;
  std::lock_guard lock(mu_);
  std::string key = tenant + "\n" + path;
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    if (entries_.size() >= max_paths_) {
      entries_.erase(order_.back());
      order_.pop_back();
    }
    order_.push_front(key);
    it = entries_.emplace(key, Entry{}).first;
    it->second.lru = order_.begin();
  } else {
    order_.splice(order_.begin(), order_, it->second.lru);
  }
  auto& e = it->second;
  if (!route.empty()) e.route = route;
  if (!box.empty()) e.box = box;
  auto& b = e.ring[size_t(at % kBuckets)];
  if (b.at != at) b = Bucket{at};
  ++b.n;
  if (status >= 500) ++b.n5;
  else if (status >= 400) ++b.n4;
  b.max = std::max(b.max, us);
  ++b.hist[size_t(Histogram::bucket(us))];

  if (from.empty() || from == path) return;
  std::string fkey = tenant + "\n" + (from.size() > 200 ? from.substr(0, 200) : from) + "\n" + path;
  auto f = flows_.find(fkey);
  if (f == flows_.end()) {
    if (flows_.size() >= max_flows_) {
      flows_.erase(flow_order_.back());
      flow_order_.pop_back();
    }
    flow_order_.push_front(fkey);
    f = flows_.emplace(fkey, FlowEntry{}).first;
    f->second.lru = flow_order_.begin();
  } else {
    flow_order_.splice(flow_order_.begin(), flow_order_, f->second.lru);
  }
  auto& slot = f->second.ring[size_t(at % kBuckets)];
  if (slot.first != at) slot = {at, 0};
  ++slot.second;
}

std::vector<LiveStats::Row> LiveStats::stats(int64_t now_ms, int minutes, const std::string& tenant,
                                             const std::string& prefix) const {
  int64_t newest = now_ms / kBucketMs;
  int64_t oldest = newest - std::clamp(minutes, 5, 60) / 5 + 1;
  std::vector<Row> out;
  std::lock_guard lock(mu_);
  for (const auto& [key, e] : entries_) {
    auto nl = key.find('\n');
    std::string t = key.substr(0, nl), path = key.substr(nl + 1);
    if (!tenant.empty() && t != tenant) continue;
    if (!prefix.empty() && !path.starts_with(prefix)) continue;
    Row r{t, path, e.route, e.box};
    Histogram h;
    for (const auto& b : e.ring) {
      if (b.at < oldest || b.at > newest) continue;
      r.n += b.n;
      r.n4 += b.n4;
      r.n5 += b.n5;
      r.max_us = std::max(r.max_us, b.max);
      for (size_t i = 0; i < b.hist.size(); ++i) h.counts[i] += b.hist[i];
    }
    if (r.n == 0) continue;
    r.p50_us = h.percentile(0.5);
    r.p95_us = h.percentile(0.95);
    r.p99_us = h.percentile(0.99);
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<LiveStats::Flow> LiveStats::flows(int64_t now_ms, int minutes, const std::string& tenant,
                                              const std::string& path) const {
  int64_t newest = now_ms / kBucketMs;
  int64_t oldest = newest - std::clamp(minutes, 5, 60) / 5 + 1;
  std::vector<Flow> out;
  std::lock_guard lock(mu_);
  for (const auto& [key, f] : flows_) {
    auto a = key.find('\n'), b = key.find('\n', a + 1);
    if (key.substr(0, a) != tenant) continue;
    std::string from = key.substr(a + 1, b - a - 1), to = key.substr(b + 1);
    if (!path.empty() && from != path && to != path) continue;
    int64_t n = 0;
    for (const auto& [at, count] : f.ring) {
      if (at >= oldest && at <= newest) n += count;
    }
    if (n > 0) out.push_back(Flow{from, to, n});
  }
  std::sort(out.begin(), out.end(), [](const Flow& x, const Flow& y) { return x.n > y.n; });
  return out;
}

size_t LiveStats::paths() const {
  std::lock_guard lock(mu_);
  return entries_.size();
}

}  // namespace goober::operations
