#include "goober/operations/pulse.h"

#include <algorithm>

namespace goober::operations {

Pulse::Minute& Pulse::minute(int64_t now_ms) {
  int64_t at = now_ms / kMinuteMs;
  auto& m = ring_[size_t(at % kMinutes)];
  if (m.at != at) m = Minute{at, {}, {}, 0};
  return m;
}

void Pulse::add(int64_t now_ms, Counter c, int64_t n) {
  std::lock_guard lock(mu_);
  minute(now_ms).n[c] += n;
}

void Pulse::request(int64_t now_ms, int status, int64_t us) {
  std::lock_guard lock(mu_);
  auto& m = minute(now_ms);
  ++m.n[kRequests];
  if (status >= 400 && status < 500) ++m.n[kRequests4xx];
  if (status >= 500) ++m.n[kRequests5xx];
  m.hist.add(us);
  m.max_us = std::max(m.max_us, us);
}

Pulse::Snapshot Pulse::snapshot(int64_t now_ms) const {
  int64_t at = now_ms / kMinuteMs;
  Snapshot s;
  Histogram h;
  std::lock_guard lock(mu_);
  for (const auto& m : ring_) {
    if (m.at < 0 || m.at > at || m.at <= at - kMinutes) continue;
    for (int i = 0; i < kCounters; ++i) s.n[size_t(i)] += m.n[size_t(i)];
    h.merge(m.hist);
    s.max_us = std::max(s.max_us, m.max_us);
  }
  s.p50_us = h.percentile(0.5);
  s.p99_us = h.percentile(0.99);
  return s;
}

}  // namespace goober::operations
