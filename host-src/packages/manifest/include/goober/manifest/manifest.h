#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/router/path.h"

namespace goober::manifest {

struct HttpRoute {
  std::string verb;  // get, post, put, patch, delete
  std::string name;  // the route name; the box exports goober_route_$name
  router::RoutePattern pattern;
  std::string export_name() const { return "goober_route_" + name; }
};

struct Behavior {
  std::optional<int64_t> timeout_ms;
  std::optional<int64_t> fuel;
  std::optional<int64_t> memory_max_bytes;
};

// The v1 subset of a box manifest (design/machine/005.FirstVersion.md §10).
struct Manifest {
  std::string box;
  std::string mission;
  std::vector<std::string> domains;  // "*" or lowercase domain names
  Behavior behavior;
  std::vector<HttpRoute> routes;
  std::vector<std::string> warnings;  // fields v1 accepts but does not implement

  bool applies_to(std::string_view domain) const;
};

// The URL prefixes no box may declare a route under.
const std::vector<std::string>& reserved_prefixes();

// Parses and checks a manifest. On failure returns nullopt with `error` set.
std::optional<Manifest> parse_manifest(std::string_view text, std::string* error);

}  // namespace goober::manifest
