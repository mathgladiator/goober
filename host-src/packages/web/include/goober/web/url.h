#pragma once

// Absolute http(s) URLs for the crawler (design/machine/017.Crawl.md): parsing, resolving a
// reference against a base (RFC 3986 §5.2), and the one normal form used as a page's key.

#include <optional>
#include <string>
#include <string_view>

namespace goober::web {

struct Url {
  std::string scheme;  // http or https, lowercase
  std::string host;    // lowercase; an IPv6 address keeps its brackets
  int port = 0;        // 0 for the scheme's default
  std::string path;    // starts with /, dot segments removed
  std::string query;   // without the ?, or empty

  // scheme://host[:port]/path[?query], with no fragment: the page's key.
  std::string str() const;
  // scheme://host[:port], for robots.txt and politeness, which are per origin.
  std::string origin() const;
  int effective_port() const { return port ? port : (scheme == "https" ? 443 : 80); }
  // The path and query, as sent in a request line.
  std::string target() const { return query.empty() ? path : path + "?" + query; }
};

// Parses an absolute http or https URL. Refuses user info (user:pass@), spaces and control
// bytes, and hosts that aren't names or addresses. Bytes outside ASCII are percent-encoded.
std::optional<Url> parse_url(std::string_view text);

// Resolves a reference (as found in an href) against a base: absolute, scheme-relative,
// absolute path, relative path, or query only. nullopt when it isn't http(s) after all.
std::optional<Url> resolve(const Url& base, std::string_view reference);

// The same site, loosely: equal hosts, or hosts that differ only by a leading "www.".
bool same_site(const Url& a, const Url& b);

}  // namespace goober::web
