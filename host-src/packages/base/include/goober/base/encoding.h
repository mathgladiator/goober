#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace goober::base {

std::string hex_encode(std::string_view bytes);

std::string base64_encode(std::string_view bytes);

// base64url without padding (RFC 4648 §5), as PKCE and URL-safe tokens use it.
std::string base64url_encode(std::string_view bytes);

// Decodes standard base64, with or without padding. Whitespace is ignored.
// Returns nullopt on any other character.
std::optional<std::string> base64_decode(std::string_view text);

// Decodes %XX escapes. Returns nullopt for a malformed escape.
std::optional<std::string> percent_decode(std::string_view text);

// Escapes every byte except A-Z a-z 0-9 and "-._~" as %XX, for a query value.
std::string percent_encode(std::string_view text);

// Escapes & < > " ' for HTML text and attribute values.
std::string html_escape(std::string_view text);

std::string to_lower(std::string_view text);

}  // namespace goober::base
