#pragma once

// Small HTTP checks shared by the host's own endpoints (identity, attachments).

#include <string>
#include <string_view>

#include "goober/base/encoding.h"
#include "goober/server_http/server.h"

namespace goober::host {

// Only a path on this site: no scheme, no "//host", no header injection.
inline std::string safe_return(std::string_view r) {
  if (r.empty() || r.size() > 2048 || r[0] != '/' || r.starts_with("//") || r.starts_with("/\\")) return "/";
  for (char c : r) {
    if (c == '\r' || c == '\n' || c == '\0') return "/";
  }
  return std::string(r);
}

// A POST must come from a page on the same host, when the browser says where it came from.
inline bool same_origin(const server::http::Request& req) {
  std::string origin = req.header("origin");
  if (origin.empty()) return true;
  auto scheme = origin.find("://");
  if (scheme == std::string::npos) return false;
  return base::to_lower(origin.substr(scheme + 3)) == base::to_lower(req.header("host"));
}

// For the consent and device screens, where a forged POST would hand out tokens: the
// browser must say where the form came from, and it must be this host.
inline bool same_origin_strict(const server::http::Request& req) {
  if (!req.header("origin").empty()) return same_origin(req);
  return req.header("sec-fetch-site") == "same-origin";
}

}  // namespace goober::host
