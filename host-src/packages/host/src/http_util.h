#pragma once

// Small HTTP checks shared by the host's own endpoints (identity, attachments).

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goober/base/encoding.h"
#include "goober/server_http/server.h"

namespace goober::host {

// Only a path on this site: no scheme, no "//host", and no byte a browser would strip or
// reinterpret (tabs, spaces, controls, backslashes), so "/\t/evil.com" can't become "//evil.com".
// A fragment ("/docs/guide#install", so a deep link survives signing in) is kept when it is at
// most 256 of RFC 3986's fragment characters; any other fragment is dropped, the path kept.
inline std::string safe_return(std::string_view r) {
  std::string_view path = r.substr(0, r.find('#'));
  std::string_view fragment = r.size() > path.size() ? r.substr(path.size() + 1) : std::string_view();
  if (path.empty() || path.size() > 2048 || path[0] != '/' || path.starts_with("//")) return "/";
  for (char c : path) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7f || c == '\\') return "/";
  }
  bool keep = !fragment.empty() && fragment.size() <= 256;
  for (char c : fragment) {
    // unreserved, pct-encoded's "%", sub-delims, ":", "@", "/", "?" (RFC 3986 § 3.5)
    keep = keep && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    std::string_view("-._~%!$&'()*+,;=:@/?").find(c) != std::string_view::npos);
  }
  return keep ? std::string(path) + "#" + std::string(fragment) : std::string(path);
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
// WebKit (Safari, every iOS browser and in-app sign-in, macOS apps' web views) sends
// Sec-Fetch-Mode and -Dest but never Sec-Fetch-User (WebKit bug 247697), so for it a
// top-level form navigation is the most it can show. Chromium and Firefox always mark a
// click with "?1", so for them a missing mark still means a script submitted the form.
inline bool user_submitted(const server::http::Request& req, const std::string& origin) {
  if (!same_origin(req, origin)) return false;
  std::string mode = req.header("sec-fetch-mode");
  if (mode.empty()) return true;
  if (mode != "navigate" || req.header("sec-fetch-dest") != "document") return false;
  std::string user = req.header("sec-fetch-user");
  if (user == "?1") return true;
  if (!user.empty()) return false;
  std::string agent = req.header("user-agent");
  return agent.find("Chrome/") == std::string::npos && agent.find("Firefox/") == std::string::npos;
}

// Request headers a box never sees: credentials belong to the host, and the client's address
// (from the proxy in front) stays with the host too, which gives boxes client_key instead.
inline bool hidden_from_box(std::string_view name) {
  return name == "authorization" || name == "cookie" || name.starts_with("x-forwarded-") || name == "forwarded" ||
         name == "x-real-ip" || name == "via" || name == "cf-connecting-ip" || name == "true-client-ip" ||
         name.starts_with("goober-");
}

// The status vocabulary (037) for a box's answer: 5xx belongs to the host, and a redirect may
// only go to a path on this site. Why the answer is refused, or "" when it stands.
inline std::string box_status_refusal(int status, const std::vector<std::pair<std::string, std::string>>& headers) {
  if (status >= 500) return "the box answered " + std::to_string(status) + "; 5xx belongs to the host";
  if (status >= 300 && status < 400) {
    std::string location;
    for (const auto& [k, v] : headers) {
      if (k == "location") location = v;
    }
    bool code_ok = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (!code_ok || location.empty() || safe_return(location) != location) {
      return "the box redirected to \"" + location.substr(0, 200) + "\"; a box may only redirect to a path on this site";
    }
  }
  return "";
}

}  // namespace goober::host
