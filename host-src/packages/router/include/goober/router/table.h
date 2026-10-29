#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "goober/router/path.h"

namespace goober::router {

template <typename T>
struct Match {
  const T* value = nullptr;
  std::map<std::string, std::string> params;
  bool method_not_allowed = false;  // the path matched, but not with this verb
};

// Routes by segment count; within a count, a literal beats a parameter at the
// first position where two patterns differ (design/machine/005.FirstVersion.md §8).
template <typename T>
class RouteTable {
 public:
  // Adds a route. Returns false if the same verb and shape already exist.
  bool add(const RoutePattern& pattern, const std::string& verb, T value) {
    for (const auto& e : entries_) {
      if (e.verb == verb && e.pattern.shape() == pattern.shape()) return false;
    }
    entries_.push_back(Entry{pattern, verb, std::move(value)});
    return true;
  }

  Match<T> match(const std::vector<std::string>& segments, const std::string& verb) const {
    Match<T> best;
    const Entry* chosen = nullptr;
    bool path_matched = false;
    for (const auto& e : entries_) {
      if (!fits(e.pattern, segments)) continue;
      path_matched = true;
      if (e.verb != verb) continue;
      if (!chosen || beats(e.pattern, chosen->pattern)) chosen = &e;
    }
    if (!chosen) {
      best.method_not_allowed = path_matched;
      return best;
    }
    best.value = &chosen->value;
    for (size_t i = 0; i < segments.size(); ++i) {
      const auto& s = chosen->pattern.segments[i];
      if (!s.literal) best.params[s.text] = segments[i];
    }
    return best;
  }

  // Every route whose path fits these segments, any verb, in the order added: what a match
  // chose among, for explaining a choice (route_check).
  std::vector<std::pair<std::string, const T*>> fitting(const std::vector<std::string>& segments) const {
    std::vector<std::pair<std::string, const T*>> out;
    for (const auto& e : entries_) {
      if (fits(e.pattern, segments)) out.emplace_back(e.verb, &e.value);
    }
    return out;
  }

  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    RoutePattern pattern;
    std::string verb;
    T value;
  };

  static bool fits(const RoutePattern& p, const std::vector<std::string>& segments) {
    if (p.segments.size() != segments.size()) return false;
    for (size_t i = 0; i < segments.size(); ++i) {
      const auto& s = p.segments[i];
      if (s.literal ? s.text != segments[i] : !matches_type(segments[i], s.type)) return false;
    }
    return true;
  }

  static bool beats(const RoutePattern& a, const RoutePattern& b) {
    for (size_t i = 0; i < a.segments.size(); ++i) {
      bool al = a.segments[i].literal, bl = b.segments[i].literal;
      if (al != bl) return al;
    }
    return false;
  }

  std::vector<Entry> entries_;
};

}  // namespace goober::router
