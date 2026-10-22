#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace goober::base {

std::string hex_encode(std::string_view bytes);

std::string base64_encode(std::string_view bytes);

// Decodes standard base64, with or without padding. Whitespace is ignored.
// Returns nullopt on any other character.
std::optional<std::string> base64_decode(std::string_view text);

// Decodes %XX escapes. Returns nullopt for a malformed escape.
std::optional<std::string> percent_decode(std::string_view text);

std::string to_lower(std::string_view text);

}  // namespace goober::base
