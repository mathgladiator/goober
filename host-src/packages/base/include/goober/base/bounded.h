#pragma once

// A bounded cache's room for one more entry: the expired go first, then the oldest, never all
// at once. A flood of new keys pushes out the oldest few; the entries in use stay.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace goober::base {

// Before inserting a key not in `m`, so it holds at most `max` entries: when full, drops every
// entry older than `keep_ms` (at_of(value) is when it was last set), and if it is still full,
// the oldest sixteenth (at least one). `m` is a std::map or std::unordered_map.
template <class Map, class AtOf>
void make_room(Map& m, size_t max, int64_t now_ms, int64_t keep_ms, AtOf at_of) {
  if (m.size() < max) return;
  for (auto it = m.begin(); it != m.end();) it = now_ms - at_of(it->second) >= keep_ms ? m.erase(it) : std::next(it);
  if (m.size() < max) return;
  std::vector<int64_t> ats;
  ats.reserve(m.size());
  for (const auto& kv : m) ats.push_back(at_of(kv.second));
  size_t drop = std::max<size_t>(1, m.size() / 16) + (m.size() - std::min(m.size(), max));
  drop = std::min(drop, ats.size());
  std::nth_element(ats.begin(), ats.begin() + std::ptrdiff_t(drop - 1), ats.end());
  int64_t cutoff = ats[drop - 1];
  // Older than the cutoff all go; of those set at the cutoff, only as many as make up `drop`,
  // so entries set in the same millisecond never empty the cache together.
  size_t older = size_t(std::count_if(ats.begin(), ats.end(), [&](int64_t a) { return a < cutoff; }));
  size_t ties = drop - older;
  for (auto it = m.begin(); it != m.end();) {
    int64_t at = at_of(it->second);
    if (at < cutoff || (at == cutoff && ties > 0)) {
      if (at == cutoff) --ties;
      it = m.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace goober::base
