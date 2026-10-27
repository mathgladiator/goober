#include "goober/web/url.h"

#include <cctype>
#include <vector>

namespace goober::web {
namespace {

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

// Bytes a URL can't carry as they are become %XX; existing escapes are kept.
std::optional<std::string> encode_part(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f) return std::nullopt;  // control bytes: not a URL
    if (c == ' ' || c >= 0x80 || c == '"' || c == '<' || c == '>' || c == '`' || c == '{' || c == '}' || c == '|' ||
        c == '\\' || c == '^') {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += char(c);
    }
  }
  return out;
}

// RFC 3986 §5.2.4: removes . and .. segments from a path that starts with /.
std::string remove_dot_segments(std::string_view path) {
  std::vector<std::string_view> out;
  std::string_view rest = path.substr(1);
  while (true) {
    size_t slash = rest.find('/');
    std::string_view seg = rest.substr(0, slash);
    bool last = slash == std::string_view::npos;
    if (seg == "..") {
      if (!out.empty()) out.pop_back();
      if (last) out.push_back("");
    } else if (seg == ".") {
      if (last) out.push_back("");
    } else {
      out.push_back(seg);
    }
    if (last) break;
    rest.remove_prefix(slash + 1);
  }
  std::string joined;
  for (size_t i = 0; i < out.size(); ++i) {
    joined += '/';
    joined += out[i];
  }
  return joined.empty() ? "/" : joined;
}

bool valid_host(std::string_view h) {
  if (h.empty() || h.size() > 253) return false;
  if (h.front() == '[') {
    if (h.back() != ']' || h.size() < 4) return false;
    for (char c : h.substr(1, h.size() - 2)) {
      if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.') return false;
    }
    return true;
  }
  for (char c : h) {
    if (!(is_alpha(c) || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')) return false;
  }
  return h.front() != '.' && h.front() != '-';
}

}  // namespace

std::string Url::origin() const {
  std::string out = scheme + "://" + host;
  if (port) out += ":" + std::to_string(port);
  return out;
}

std::string Url::str() const {
  std::string out = origin() + path;
  if (!query.empty()) out += "?" + query;
  return out;
}

std::optional<Url> parse_url(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  if (text.size() > 8192) return std::nullopt;
  auto sep = text.find("://");
  if (sep == std::string_view::npos) return std::nullopt;
  Url u;
  for (char c : text.substr(0, sep)) u.scheme += lower(c);
  if (u.scheme != "http" && u.scheme != "https") return std::nullopt;
  std::string_view rest = text.substr(sep + 3);
  if (auto hash = rest.find('#'); hash != std::string_view::npos) rest = rest.substr(0, hash);
  size_t end = rest.find_first_of("/?");
  std::string_view authority = rest.substr(0, end);
  rest = end == std::string_view::npos ? std::string_view() : rest.substr(end);
  if (authority.find('@') != std::string_view::npos) return std::nullopt;
  std::string_view host = authority, port;
  if (!authority.empty() && authority.front() == '[') {
    auto close = authority.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    host = authority.substr(0, close + 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') return std::nullopt;
      port = authority.substr(close + 2);
    }
  } else if (auto colon = authority.find(':'); colon != std::string_view::npos) {
    host = authority.substr(0, colon);
    port = authority.substr(colon + 1);
  }
  for (char c : host) u.host += lower(c);
  while (!u.host.empty() && u.host.back() == '.') u.host.pop_back();  // "example.com." is example.com
  if (!valid_host(u.host)) return std::nullopt;
  if (!port.empty()) {
    if (port.size() > 5) return std::nullopt;
    int p = 0;
    for (char c : port) {
      if (c < '0' || c > '9') return std::nullopt;
      p = p * 10 + (c - '0');
    }
    if (p < 1 || p > 65535) return std::nullopt;
    if (!((u.scheme == "http" && p == 80) || (u.scheme == "https" && p == 443))) u.port = p;
  }
  std::string_view path = rest, query;
  if (auto q = rest.find('?'); q != std::string_view::npos) {
    path = rest.substr(0, q);
    query = rest.substr(q + 1);
  }
  auto p = encode_part(path);
  auto qe = encode_part(query);
  if (!p || !qe) return std::nullopt;
  u.path = remove_dot_segments(p->empty() || p->front() != '/' ? "/" + *p : *p);
  u.query = *qe;
  return u;
}

std::optional<Url> resolve(const Url& base, std::string_view ref) {
  while (!ref.empty() && std::isspace(static_cast<unsigned char>(ref.front()))) ref.remove_prefix(1);
  while (!ref.empty() && std::isspace(static_cast<unsigned char>(ref.back()))) ref.remove_suffix(1);
  if (auto hash = ref.find('#'); hash != std::string_view::npos) ref = ref.substr(0, hash);
  if (ref.empty()) return base;
  // A scheme: letters, digits, + - . before a colon that comes before any / ? #.
  size_t colon = ref.find(':');
  size_t first_delim = ref.find_first_of("/?#");
  if (colon != std::string_view::npos && colon > 0 && (first_delim == std::string_view::npos || colon < first_delim) &&
      is_alpha(ref[0])) {
    bool scheme = true;
    for (char c : ref.substr(0, colon)) scheme &= is_alpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (scheme) return parse_url(ref);
  }
  if (ref.starts_with("//")) return parse_url(base.scheme + ":" + std::string(ref));
  std::string origin = base.origin();
  if (ref.front() == '/') return parse_url(origin + std::string(ref));
  if (ref.front() == '?') return parse_url(origin + base.path + std::string(ref));
  std::string dir = base.path.substr(0, base.path.rfind('/') + 1);
  return parse_url(origin + dir + std::string(ref));
}

bool same_site(const Url& a, const Url& b) {
  auto bare = [](std::string_view h) { return h.starts_with("www.") ? h.substr(4) : h; };
  return bare(a.host) == bare(b.host);
}

}  // namespace goober::web
