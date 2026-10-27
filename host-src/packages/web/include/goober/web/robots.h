#pragma once

// robots.txt (RFC 9309), as the crawler reads it: the group for our product token, or else
// the * group; Allow and Disallow by longest match, with * and $; Crawl-delay; Sitemap lines.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace goober::web {

class Robots {
 public:
  // Everything allowed: what a 4xx robots.txt means (RFC 9309 §2.3.1.3).
  static Robots allow_all();
  // Nothing allowed: what an unreachable or 5xx robots.txt means.
  static Robots disallow_all();
  // Parses a robots.txt body for the product token (like "goober"), case-insensitively.
  static Robots parse(std::string_view text, std::string_view product);

  // Whether a path (with its query) may be fetched.
  bool allowed(std::string_view target) const;
  // The site's Crawl-delay for us, in milliseconds, or 0.
  int64_t crawl_delay_ms() const { return delay_ms_; }
  const std::vector<std::string>& sitemaps() const { return sitemaps_; }

 private:
  struct Rule {
    bool allow;
    std::string pattern;
  };
  std::vector<Rule> rules_;
  std::vector<std::string> sitemaps_;
  int64_t delay_ms_ = 0;
  bool all_disallowed_ = false;
};

// Whether a robots.txt pattern (with * and a trailing $) matches a target.
bool robots_match(std::string_view pattern, std::string_view target);

}  // namespace goober::web
