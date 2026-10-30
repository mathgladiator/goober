#pragma once

// The host's logs (design/machine/032.Logs.md): one JSON object per line, enveloped (at, log,
// seq, host) and chained (each line's chain is a hash over the previous chain and the line), in
// files that rotate daily or by size into name.1, name.2, ..., with a hard link and a manifest
// queued under archive/ for upload. Verification and search read the local files.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"

namespace goober::logs {

namespace bj = boost::json;

inline constexpr uint64_t kMaxBytes = 256ull << 20;
inline constexpr int kKeepFiles = 64;
inline constexpr std::string_view kGenesis = "genesis";

// The chain of a line: the first 32 hex digits of SHA-256 over the previous chain, "\n", and
// the line's bytes without its chain field.
std::string chain_of(std::string_view prev, std::string_view body);

// The line written for a body ("{...}"): the body with ,"chain":"..." before its last brace.
std::string chained_line(std::string_view body, std::string_view chain);
// Splits a written line into its body and chain; nullopt for a line that has none.
std::optional<std::pair<std::string, std::string>> split_chain(std::string_view line);

// A rotated file, queued for upload: its hard link under archive/, and its manifest.
struct Rotated {
  std::string log;
  std::filesystem::path queued;    // archive/$name-$stamp.log
  std::filesystem::path manifest;  // archive/$name-$stamp.manifest.json
  bj::object manifest_json;
};

// One log file and its rotations. Thread-safe; other processes may write it too.
class Writer {
 public:
  Writer(std::filesystem::path file, std::string name, std::string host, uint64_t max_bytes = kMaxBytes);
  // Appends one line: at, log, seq, and host first, then `fields` (theirs never overwrite the
  // envelope), then the chain. Rotates first when the file is from an earlier UTC day or the
  // line would pass max_bytes. Returns what it rotated, if anything.
  std::optional<Rotated> write(const bj::object& fields, int64_t now_ms, bool* ok = nullptr);
  // Rotates a non-empty file from an earlier UTC day (the hourly tick, for quiet logs).
  std::optional<Rotated> rotate_if_stale(int64_t now_ms);
  const std::string& name() const { return name_; }
  const std::filesystem::path& file() const { return file_; }

 private:
  struct Tail;
  std::optional<Rotated> rotate_locked(int fd, int64_t now_ms);
  std::filesystem::path file_;
  std::string name_, host_;
  uint64_t max_bytes_;
  std::mutex mu_;
  // What this writer last saw of the file: unchanged inode and size mean nobody else wrote.
  uint64_t seen_ino_ = 0;
  int64_t seen_size_ = -1;
  std::string seen_chain_, seen_day_;
  int64_t seen_seq_ = 0;
};

// Every log under a directory, by name ("access", "boxes/shop").
class Set {
 public:
  Set(std::filesystem::path dir, std::string host, uint64_t max_bytes = kMaxBytes);
  std::optional<Rotated> write(const std::string& name, const bj::object& fields, int64_t now_ms);
  std::vector<Rotated> rotate_stale(int64_t now_ms);
  // The logs on disk: every name with a current or rotated file.
  std::vector<std::string> names() const;
  const std::filesystem::path& dir() const { return dir_; }

 private:
  Writer& writer(const std::string& name);
  std::filesystem::path dir_;
  std::string host_;
  uint64_t max_bytes_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<Writer>> writers_;
};

// A log's files, oldest first: name.N ... name.1, name.
std::vector<std::filesystem::path> files_of(const std::filesystem::path& dir, const std::string& name);
// Whether a log name is one the host writes: [a-z]+, or boxes/ and a box name.
bool valid_name(std::string_view name);

struct Verified {
  bool ok = true;
  int64_t lines = 0, unchained = 0, files = 0;
  std::string first_seq, last_seq;
  std::string broken;  // where the chain breaks: file and line number, and why
};
// Recomputes every chain in the log's local files, oldest first. Lines written before chains
// existed are counted, not checked.
Verified verify(const std::filesystem::path& dir, const std::string& name);

struct Query {
  std::string pattern;                         // a substring of the raw line, or ""
  std::optional<std::regex> regex;             // or a regex over it
  std::map<std::string, std::string> fields;   // top-level fields that must equal (as text)
  std::vector<std::string> hashes;             // any *_h field equal to one of these
  int64_t since_ms = 0, until_ms = INT64_MAX;
  size_t limit = 200;
  // Resume below this line: lines of `before_log` with seq < before_seq, and every other log's
  // lines older than before_at_ms.
  std::string before_log;
  int64_t before_seq = 0, before_at_ms = 0;
  uint64_t max_read = 2ull << 30;
  std::function<bool(const std::string& log, const bj::object& line)> allow;  // scope
};
struct Hit {
  std::string log;
  int64_t seq = 0, at_ms = 0;
  std::string line;
};
struct Found {
  std::vector<Hit> hits;  // newest first
  bool truncated = false;  // stopped at limit or max_read
  uint64_t bytes_read = 0;
};
Found search(const std::filesystem::path& dir, const std::vector<std::string>& names, const Query& query);

// "2026-10-29T12:00:00.000Z" as unix ms, or nullopt.
std::optional<int64_t> parse_at(std::string_view at);

}  // namespace goober::logs
