#include "goober/web/robots.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace goober::web {
namespace {

constexpr size_t kMaxBytes = 512 << 10;  // RFC 9309 asks crawlers to read at least 500 KiB
constexpr size_t kMaxRules = 1000;
constexpr size_t kMaxPattern = 512;

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

}  // namespace

bool robots_match(std::string_view pattern, std::string_view target) {
  bool anchored = !pattern.empty() && pattern.back() == '$';
  if (anchored) pattern.remove_suffix(1);
  // Iterative wildcard matching against a prefix of the target (or all of it, anchored).
  size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
  while (t < target.size()) {
    if (p == pattern.size() && !anchored) return true;
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (p < pattern.size() && pattern[p] == target[t]) {
      ++p;
      ++t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

Robots Robots::allow_all() { return Robots(); }

Robots Robots::disallow_all() {
  Robots r;
  r.all_disallowed_ = true;
  return r;
}

Robots Robots::parse(std::string_view text, std::string_view product_in) {
  text = text.substr(0, kMaxBytes);
  std::string product = lower(product_in);
  struct Group {
    std::vector<std::string> agents;
    std::vector<Rule> rules;
    int64_t delay_ms = 0;
  };
  std::vector<Group> groups;
  Robots out;
  bool in_agents = false;  // consecutive user-agent lines share a group
  size_t i = 0;
  while (i < text.size()) {
    size_t eol = text.find_first_of("\r\n", i);
    std::string_view line = text.substr(i, eol == std::string_view::npos ? std::string_view::npos : eol - i);
    i = eol == std::string_view::npos ? text.size() : eol + 1;
    if (auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    std::string key = lower(trim(line.substr(0, colon)));
    std::string_view value = trim(line.substr(colon + 1));
    if (key == "user-agent") {
      if (!in_agents) groups.emplace_back();
      in_agents = true;
      groups.back().agents.push_back(lower(value));
      continue;
    }
    if (key == "sitemap") {
      if (out.sitemaps_.size() < 50 && !value.empty()) out.sitemaps_.emplace_back(value);
      continue;
    }
    in_agents = false;
    if (groups.empty()) continue;  // rules before any user-agent belong to nobody
    if (key == "allow" || key == "disallow") {
      if (value.empty() || value.size() > kMaxPattern) continue;  // "Disallow:" allows everything
      if (groups.back().rules.size() < kMaxRules) groups.back().rules.push_back(Rule{key == "allow", std::string(value)});
    } else if (key == "crawl-delay") {
      double seconds = std::strtod(std::string(value).c_str(), nullptr);
      if (seconds > 0) groups.back().delay_ms = int64_t(std::min(seconds, 60.0) * 1000);
    }
  }
  // Our own groups, merged; else the * groups (RFC 9309 §2.2.1).
  auto take = [&](auto pred) {
    bool any = false;
    for (const auto& g : groups) {
      if (!std::any_of(g.agents.begin(), g.agents.end(), pred)) continue;
      any = true;
      for (const auto& r : g.rules) {
        if (out.rules_.size() < kMaxRules) out.rules_.push_back(r);
      }
      out.delay_ms_ = std::max(out.delay_ms_, g.delay_ms);
    }
    return any;
  };
  if (!take([&](const std::string& a) { return !product.empty() && a == product; })) {
    take([](const std::string& a) { return a == "*"; });
  }
  return out;
}

bool Robots::allowed(std::string_view target) const {
  if (target == "/robots.txt") return true;
  if (all_disallowed_) return false;
  size_t best = 0;
  bool allow = true;
  for (const auto& r : rules_) {
    if (!robots_match(r.pattern, target)) continue;
    // The longest pattern wins; on a tie, allow (RFC 9309 §2.2.2).
    if (r.pattern.size() > best || (r.pattern.size() == best && r.allow)) {
      best = r.pattern.size();
      allow = r.allow;
    }
  }
  return allow;
}

}  // namespace goober::web
