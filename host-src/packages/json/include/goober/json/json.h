#pragma once

// Boost.JSON, compiled once in this package, with small helpers for reading
// typed fields with clear errors.

#include <boost/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace goober::json {

namespace bj = boost::json;

// Parses text, returning nullopt and setting `error` on failure.
std::optional<bj::value> parse(std::string_view text, std::string* error);

// Serializes compactly.
std::string dump(const bj::value& value);

// Serializes with two-space indentation, for files people read.
std::string pretty(const bj::value& value);

// Field readers. Each returns nullopt when the field is missing; when the field
// is present with the wrong type, it returns nullopt and sets `error`.
std::optional<std::string> get_string(const bj::object& obj, std::string_view key, std::string* error);
std::optional<int64_t> get_int(const bj::object& obj, std::string_view key, std::string* error);
std::optional<bool> get_bool(const bj::object& obj, std::string_view key, std::string* error);
const bj::object* get_object(const bj::object& obj, std::string_view key, std::string* error);
const bj::array* get_array(const bj::object& obj, std::string_view key, std::string* error);

}  // namespace goober::json
