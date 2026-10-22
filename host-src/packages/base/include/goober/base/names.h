#pragma once

#include <string_view>

namespace goober::base {

// The longest name the host accepts.
inline constexpr size_t kMaxNameLength = 63;

// Tenant, box, and route names: [a-z][a-z0-9_]*, at most 63 characters, and not
// starting with "goober_", which generated code owns.
bool is_valid_name(std::string_view name);

}  // namespace goober::base
