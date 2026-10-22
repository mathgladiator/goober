#include "goober/router/path.h"

#include <cctype>
#include <charconv>

#include "goober/base/encoding.h"

namespace goober::router {

std::optional<NormalizedPath> normalize_path(std::string_view raw) {
  if (raw.empty() || raw.front() != '/') return std::nullopt;
  NormalizedPath out;
  size_t i = 0;
  while (i < raw.size()) {
    while (i < raw.size() && raw[i] == '/') ++i;
    if (i >= raw.size()) break;
    size_t end = raw.find('/', i);
    if (end == std::string_view::npos) end = raw.size();
    auto decoded = base::percent_decode(raw.substr(i, end - i));
    if (!decoded || decoded->empty() || *decoded == "." || *decoded == "..") return std::nullopt;
    for (char c : *decoded) {
      if (c == '/' || c == '\0') return std::nullopt;
    }
    out.segments.push_back(std::move(*decoded));
    i = end;
  }
  if (out.segments.empty()) {
    out.path = "/";
  } else {
    for (const auto& s : out.segments) out.path += "/" + s;
  }
  return out;
}

std::map<std::string, std::string> parse_query(std::string_view query) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i <= query.size() && !query.empty()) {
    size_t end = query.find('&', i);
    if (end == std::string_view::npos) end = query.size();
    std::string_view pair = query.substr(i, end - i);
    if (!pair.empty()) {
      auto eq = pair.find('=');
      std::string k(pair.substr(0, eq));
      std::string v(eq == std::string_view::npos ? "" : pair.substr(eq + 1));
      for (auto& c : k) c = c == '+' ? ' ' : c;
      for (auto& c : v) c = c == '+' ? ' ' : c;
      auto dk = base::percent_decode(k);
      auto dv = base::percent_decode(v);
      if (dk && dv && !dk->empty()) out[*dk] = *dv;
    }
    if (end == query.size()) break;
    i = end + 1;
  }
  return out;
}

std::string RoutePattern::shape() const {
  std::string out;
  for (const auto& s : segments) out += s.literal ? "/" + s.text : "/{}";
  return out.empty() ? "/" : out;
}

namespace {
bool is_identifier(std::string_view s) {
  if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  }
  return true;
}
std::string_view trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return s;
}
}  // namespace

std::optional<RoutePattern> parse_pattern(std::string_view source, std::string* error) {
  RoutePattern p;
  p.source = std::string(source);
  if (source.empty() || source.front() != '/') {
    *error = "a route path must start with \"/\": " + p.source;
    return std::nullopt;
  }
  if (source == "/") return p;
  std::string_view rest = source.substr(1);
  while (true) {
    size_t end = 0;
    if (!rest.empty() && rest.front() == '{') {
      end = rest.find('}');
      if (end == std::string_view::npos) {
        *error = "unclosed \"{\" in " + p.source;
        return std::nullopt;
      }
      std::string_view inner = rest.substr(1, end - 1);
      auto colon = inner.find(':');
      Segment seg;
      seg.literal = false;
      seg.text = std::string(trim(inner.substr(0, colon)));
      std::string type = colon == std::string_view::npos ? "string" : std::string(trim(inner.substr(colon + 1)));
      if (!is_identifier(seg.text) || seg.text.starts_with("goober_") || seg.text.starts_with("!")) {
        *error = "bad parameter name \"" + seg.text + "\" in " + p.source;
        return std::nullopt;
      }
      if (type == "string") {
        seg.type = ParamType::kString;
      } else if (type == "int32") {
        seg.type = ParamType::kInt32;
      } else if (type == "int64") {
        seg.type = ParamType::kInt64;
      } else {
        *error = "unknown parameter type \"" + type + "\" in " + p.source + " (string, int32, int64)";
        return std::nullopt;
      }
      for (const auto& s : p.segments) {
        if (!s.literal && s.text == seg.text) {
          *error = "parameter \"" + seg.text + "\" appears twice in " + p.source;
          return std::nullopt;
        }
      }
      p.segments.push_back(std::move(seg));
      ++end;
      if (end < rest.size() && rest[end] != '/') {
        *error = "a parameter must fill a whole segment in " + p.source;
        return std::nullopt;
      }
    } else {
      end = rest.find('/');
      if (end == std::string_view::npos) end = rest.size();
      std::string_view lit = rest.substr(0, end);
      if (lit.empty()) {
        *error = "empty segment in " + p.source;
        return std::nullopt;
      }
      for (char c : lit) {
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' || c == '~';
        if (!ok) {
          *error = "bad character in route segment \"" + std::string(lit) + "\" in " + p.source;
          return std::nullopt;
        }
      }
      if (lit == "." || lit == "..") {
        *error = "\".\" and \"..\" can't be route segments: " + p.source;
        return std::nullopt;
      }
      p.segments.push_back(Segment{true, std::string(lit), ParamType::kString});
    }
    if (end >= rest.size()) break;
    rest = rest.substr(end + 1);
    if (rest.empty()) {
      *error = "a route path can't end with \"/\": " + p.source;
      return std::nullopt;
    }
  }
  return p;
}

bool matches_type(std::string_view value, ParamType type) {
  if (type == ParamType::kString) return !value.empty();
  if (value.empty()) return false;
  size_t start = value.front() == '-' ? 1 : 0;
  if (start == value.size()) return false;
  for (size_t i = start; i < value.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  if (type == ParamType::kInt64) {
    int64_t v = 0;
    auto r = std::from_chars(value.data(), value.data() + value.size(), v);
    return r.ec == std::errc() && r.ptr == value.data() + value.size();
  }
  int32_t v = 0;
  auto r = std::from_chars(value.data(), value.data() + value.size(), v);
  return r.ec == std::errc() && r.ptr == value.data() + value.size();
}

}  // namespace goober::router
