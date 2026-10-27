#pragma once

// Small HTTP checks shared by the host's own endpoints (identity, attachments).

#include <string>
#include <string_view>

#include "goober/base/encoding.h"
#include "goober/server_http/server.h"

namespace goober::host {

// Only a path on this site: no scheme, no "//host", and no byte a browser would strip or
// reinterpret (tabs, spaces, controls, backslashes), so "/\t/evil.com" can't become "//evil.com".
inline std::string safe_return(std::string_view r) {
  if (r.empty() || r.size() > 2048 || r[0] != '/' || r.starts_with("//")) return "/";
  for (char c : r) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f || c == '\\') return "/";
  }
  return std::string(r);
}

// A browser's state-changing request came from a page of this origin (scheme, host, and
// port, like "https://example.com"): its Origin header says so, or, without one, its
// Sec-Fetch-Site does. Every current browser sends one of them on a POST. A request that
// says neither and carries no cookie isn't a browser acting for someone, so it passes;
// one that carries a cookie is refused.
inline bool same_origin(const server::http::Request& req, const std::string& origin) {
  std::string o = req.header("origin");
  if (!o.empty()) return base::to_lower(o) == base::to_lower(origin);
  std::string site = req.header("sec-fetch-site");
  if (!site.empty()) return site == "same-origin";
  return req.header("cookie").empty();
}

// For the consent and device screens, where a forged POST would hand out tokens: besides
// coming from this origin, the POST must be a person's own navigation, a form they
// submitted by clicking, not a script's fetch. Browsers set Sec-Fetch-* themselves, and no
// script can forge them; a browser too old to send them is held to the origin check alone.
inline bool user_submitted(const server::http::Request& req, const std::string& origin) {
  if (!same_origin(req, origin)) return false;
  std::string mode = req.header("sec-fetch-mode");
  if (mode.empty()) return true;
  return mode == "navigate" && req.header("sec-fetch-dest") == "document" && req.header("sec-fetch-user") == "?1";
}

}  // namespace goober::host
