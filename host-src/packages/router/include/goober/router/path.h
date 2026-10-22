#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goober::router {

struct NormalizedPath {
  std::string path;                   // "/" or "/a/b", with decoded segments
  std::vector<std::string> segments;  // decoded; empty for "/"
};

// Normalizes a request path (no query string): collapse runs of "/", trim a
// trailing "/" except at the root, then percent-decode each segment. Refuses a
// path not starting with "/", a bad escape, or a segment that decodes to "",
// ".", "..", or contains "/" or NUL.
std::optional<NormalizedPath> normalize_path(std::string_view raw);

// Splits "a=1&b=x%20y" into decoded pairs. Later duplicates win. Bad escapes are dropped.
std::map<std::string, std::string> parse_query(std::string_view query);

enum class ParamType { kString, kInt32, kInt64 };

struct Segment {
  bool literal = true;
  std::string text;  // the literal text, or the parameter name
  ParamType type = ParamType::kString;
};

struct RoutePattern {
  std::string source;
  std::vector<Segment> segments;
  // The shape for conflicts: literals in place, every parameter as "{}".
  std::string shape() const;
};

// Parses "/hello/{name : string}". Literals may hold letters, digits, and
// "-._~"; parameter names are identifiers; types are string, int32, and int64.
std::optional<RoutePattern> parse_pattern(std::string_view source, std::string* error);

// True when the decoded segment is a valid value for the type.
bool matches_type(std::string_view value, ParamType type);

}  // namespace goober::router
