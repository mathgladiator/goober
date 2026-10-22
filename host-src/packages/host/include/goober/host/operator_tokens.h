#pragma once

// Operator tokens for /goober-operations-mcp, minted by `goober-host token create`
// and kept as one file per token under $storage/operations/tokens/, named by the
// token's SHA-256. Only the hash is stored. A token acts as host:owner in v1.
// Files are written atomically, so the CLI can mint while the host runs.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goober::host {

struct OperatorToken {
  std::string id;    // the first 12 hex digits of the token's hash
  std::string name;  // a label, like claude-code
  int64_t created_ms = 0;
  std::vector<std::string> roles;
};

// A label: [a-z0-9_-], 1 to 63 characters.
bool is_valid_token_name(std::string_view name);

// Mints a token and returns it raw ("gop_" and 64 hex digits). It is shown once.
std::optional<std::string> create_operator_token(const std::filesystem::path& storage, std::string_view name,
                                                 int64_t now_ms, std::string* error);

std::vector<OperatorToken> list_operator_tokens(const std::filesystem::path& storage);

// Revokes every token whose name or id equals `which`. Returns how many.
int revoke_operator_tokens(const std::filesystem::path& storage, std::string_view which);

// The token for a raw value, or nullopt.
std::optional<OperatorToken> find_operator_token(const std::filesystem::path& storage, std::string_view raw);

}  // namespace goober::host
