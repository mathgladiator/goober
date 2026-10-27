#include "goober/server_http/multipart.h"

#include "goober/base/encoding.h"

namespace goober::server::http {

// The value of `key` in a header like: form-data; name="file"; filename="cat.png".
std::string header_param(std::string_view header, std::string_view key) {
  std::string lower = base::to_lower(header);
  std::string want = std::string(key) + "=";
  for (size_t at = lower.find(want); at != std::string::npos; at = lower.find(want, at + 1)) {
    if (at > 0 && lower[at - 1] != ' ' && lower[at - 1] != ';') continue;
    size_t v = at + want.size();
    if (v < header.size() && header[v] == '"') {
      size_t end = header.find('"', v + 1);
      if (end == std::string_view::npos) return "";
      return std::string(header.substr(v + 1, end - v - 1));
    }
    size_t end = header.find(';', v);
    return std::string(header.substr(v, end == std::string_view::npos ? std::string_view::npos : end - v));
  }
  return "";
}

// Parses a multipart/form-data body: the "file" part's bytes and filename, and every other field.
std::optional<Upload> parse_multipart(std::string_view body, std::string_view content_type, std::string* error) {
  std::string boundary = header_param(content_type, "boundary");
  if (boundary.empty()) return *error = "multipart/form-data needs a boundary", std::nullopt;
  // RFC 2046 caps a boundary at 70 characters, which also keeps every search linear.
  if (boundary.size() > 70) return *error = "a multipart boundary is at most 70 characters", std::nullopt;
  std::string delim = "--" + boundary;
  Upload up;
  bool have_file = false;
  size_t pos = body.find(delim);
  while (pos != std::string_view::npos) {
    pos += delim.size();
    if (body.substr(pos, 2) == "--") break;
    if (body.substr(pos, 2) == "\r\n") pos += 2;
    size_t head_end = body.find("\r\n\r\n", pos);
    if (head_end == std::string_view::npos) return *error = "a multipart part has no headers", std::nullopt;
    std::string_view head = body.substr(pos, head_end - pos);
    size_t data_start = head_end + 4;
    size_t next = body.find("\r\n" + delim, data_start);
    if (next == std::string_view::npos) return *error = "a multipart part never ends", std::nullopt;
    std::string_view data = body.substr(data_start, next - data_start);
    std::string disposition;
    size_t line = 0;
    while (line < head.size()) {
      size_t end = head.find("\r\n", line);
      std::string_view h = head.substr(line, end == std::string_view::npos ? std::string_view::npos : end - line);
      if (base::to_lower(h.substr(0, 20)).starts_with("content-disposition:")) disposition = std::string(h.substr(20));
      if (end == std::string_view::npos) break;
      line = end + 2;
    }
    std::string name = header_param(disposition, "name");
    if (name == "file") {
      up.bytes = std::string(data);
      up.name = header_param(disposition, "filename");
      have_file = true;
    } else if (!name.empty()) {
      up.fields[name] = std::string(data);
    }
    pos = next + 2;
  }
  if (!have_file) return *error = "the form has no \"file\" field", std::nullopt;
  return up;
}

}  // namespace goober::server::http
