#pragma once

// multipart/form-data bodies (RFC 7578), as the attachment upload form sends them.

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace goober::server::http {

struct Upload {
  std::string bytes;  // the "file" part
  std::string name;   // its filename
  std::map<std::string, std::string> fields;  // every other named part
};

// The value of `key` in a header like: form-data; name="file"; filename="cat.png".
std::string header_param(std::string_view header, std::string_view key);

// Parses a multipart/form-data body. Linear in the body; nullopt with `error` when it is
// malformed or has no "file" part.
std::optional<Upload> parse_multipart(std::string_view body, std::string_view content_type, std::string* error);

}  // namespace goober::server::http
