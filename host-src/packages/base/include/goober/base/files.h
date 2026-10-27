#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goober::base {

// True for a relative path the host may write under one of its roots: no leading
// "/", no empty, "." or ".." segment, no NUL or backslash.
bool is_safe_relative_path(std::string_view path);

std::optional<std::string> read_file(const std::filesystem::path& path);

// Writes the file durably: a temporary file is written and fsynced, renamed into
// place, and the directory is fsynced. Parent directories are created.
// Returns false on any failure.
bool write_file_durable(const std::filesystem::path& path, std::string_view data);

// Removes the file and fsyncs its directory. Removing a missing file succeeds.
bool remove_file_durable(const std::filesystem::path& path);

// Every regular file under root, as "/"-separated paths relative to root, sorted.
std::vector<std::string> list_files(const std::filesystem::path& root);

// Appends one line to the file, creating it and its parents. Not fsynced.
// Appends one line. A file past kLogRotateBytes becomes path.1 (and older ones move up,
// to path.3), so a log can't fill the disk.
inline constexpr uint64_t kLogRotateBytes = 64ull << 20;
inline constexpr int kLogsKept = 3;
bool append_line(const std::filesystem::path& path, std::string_view line, uint64_t rotate_bytes = kLogRotateBytes);

// The last `limit` lines of a file, oldest first.
std::vector<std::string> tail_lines(const std::filesystem::path& path, size_t limit);

}  // namespace goober::base
