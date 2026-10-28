#pragma once

// The pieces of backups that don't need a host (design/human/663.Backups.md;
// design/machine/019.Backups.md): sealing files before they leave the machine, page-level
// deltas of SQLite databases, and the names of what goes in the bucket.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goober::backup {

// ---- Sealing ----
//
// A sealed file is "GOOBERS1", a 16-byte random salt, the ChaCha20 ciphertext, and an
// HMAC-SHA-256 tag over everything before it (encrypt-then-MAC). Both keys come from the one
// backup key and the salt, so no two files share a key stream.

using Key = std::array<uint8_t, 32>;

// 64 hex digits, as `goober-host backup key` prints it, or nullopt.
std::optional<Key> parse_key(std::string_view hex);
std::string key_hex(const Key& key);
Key new_key();

// `context` (the object's name in the bucket) is bound into the tag but not stored: a sealed
// file moved to another name, an asset over another tenant's or an old night over a new
// one, fails its check.
bool seal_file(const Key& key, const std::filesystem::path& in, const std::filesystem::path& out, std::string* error,
               std::string_view context = {});
// Checks the tag before writing a byte: out appears only if the whole file is authentic.
bool unseal_file(const Key& key, const std::filesystem::path& in, const std::filesystem::path& out, std::string* error,
                 std::string_view context = {});

// ---- Pages ----

struct PageMap {
  uint32_t page_size = 0;
  uint64_t pages = 0;
  std::vector<std::array<uint8_t, 16>> hashes;  // one per page, truncated SHA-256
  std::string sha256;                           // of the whole file
};

// The page size from a SQLite file's header, and a hash of every page. nullopt (with error)
// when it isn't a SQLite file.
std::optional<PageMap> map_pages(const std::filesystem::path& db, std::string* error);

struct DeltaInfo {
  uint64_t pages = 0;    // in the snapshot
  uint64_t changed = 0;  // pages written to the delta
  std::string sha256;    // of the snapshot, which applying the delta must reproduce
};

// Writes the pages of `snapshot` that differ from `base` (all of them without one, or when the
// page size changed). The delta is a JSON header line, then (page number, page bytes) records.
bool write_delta(const PageMap* base, const std::filesystem::path& snapshot, const std::filesystem::path& out,
                 DeltaInfo* info, std::string* error);

// Rebuilds the snapshot: `base` (or nothing) with the delta's pages, cut to its length, into
// `out`, and checks its SHA-256.
bool apply_delta(const std::filesystem::path* base, const std::filesystem::path& delta, const std::filesystem::path& out,
                 std::string* error);

// ---- Names ----

// 2026-10-27T03-30-00Z: sortable, and safe in a key. A stamp names a moment to the second.
std::string stamp(int64_t unix_ms);
std::optional<int64_t> parse_stamp(std::string_view stamp);
// A moment a person types: 2026-10-26, 2026-10-26T18:00, 2026-10-26T18:00:00Z, or a stamp.
std::optional<int64_t> parse_moment(std::string_view text);

inline std::string nightly_key(const std::string& stamp) { return "nightly/" + stamp + ".tar.gz.sealed"; }
inline std::string increment_prefix(const std::string& night, const std::string& at) {
  return "increments/" + night + "/" + at + "/";
}
inline std::string asset_key(const std::string& tenant, int64_t id, const std::string& ext) {
  return "assets/" + tenant + "/" + std::to_string(id) + "." + ext + ".sealed";
}

// Restore points, from a bucket listing: nights (nightly/STAMP.tar.gz.sealed) and the
// increments under each (increments/NIGHT/STAMP/MANIFEST.json.sealed).
struct RestorePoint {
  std::string night;      // the nightly's stamp
  std::string increment;  // "" for the nightly itself
  int64_t at_ms = 0;
};
std::vector<RestorePoint> restore_points(const std::vector<std::string>& keys);
// The latest point at or before at_ms (any, when at_ms is 0).
std::optional<RestorePoint> choose_point(const std::vector<RestorePoint>& points, int64_t at_ms);

}  // namespace goober::backup
