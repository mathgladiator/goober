#pragma once

// Exports (design/machine/008.Databases.md § 7.1): a query's rows as a CSV sealed on the
// server, fetched by a person with `goober-host export get`, so private rows never pass through
// an agent. Shared by the db_export tool and the CLI.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "goober/vault/vault.h"

namespace goober::host::exports {

inline constexpr int64_t kMaxRows = 100'000;
inline constexpr int64_t kKeepMs = 7LL * 24 * 3600 * 1000;

struct Meta {
  std::string id, tenant, database, sql, operator_name, sha256;
  int64_t rows = 0, bytes = 0, created_ms = 0, expires_ms = 0;
  std::vector<std::string> columns;
  boost::json::object to_json() const;
};

// storage/exports: <id>.csv.sealed beside <id>.json, the record above.
std::filesystem::path dir(const std::filesystem::path& storage);

// RFC 4180: CRLF line endings, a header row, cells quoted when they hold a comma, a quote, or a
// line break. Nulls are empty cells; blobs are their base64.
std::string to_csv(const boost::json::object& result);

// Seals one query result ({columns, rows}, as Databases::operate answers) under the tenant's
// sealing key, bound to its id, and keeps its record. Nothing in the clear is written.
std::optional<Meta> write(const std::filesystem::path& storage, const vault::Key& vault_key, const std::string& tenant,
                          const std::string& database, const std::string& sql, const std::string& operator_name,
                          const boost::json::object& result, int64_t now_ms, std::string* error);

// The kept exports, newest first. Expired ones are deleted on the way.
std::vector<Meta> list(const std::filesystem::path& storage, int64_t now_ms);
std::optional<Meta> find(const std::filesystem::path& storage, const std::string& id, int64_t now_ms);

// Writes the CSV to out (mode 0600, refused if out exists), after checking the seal.
bool open_to(const std::filesystem::path& storage, const vault::Key& vault_key, const std::string& id,
             const std::filesystem::path& out, int64_t now_ms, std::string* error);

bool remove(const std::filesystem::path& storage, const std::string& id);

}  // namespace goober::host::exports
