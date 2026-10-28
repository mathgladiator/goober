#pragma once

// Runs a program the host trusts (curl, for fetching and for mail), with an argument
// vector and never a shell, so no value is ever parsed as a command.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goober::base {

struct ProcessResult {
  bool started = false;    // false when the program couldn't be run at all
  int exit_code = -1;      // -1 when it was killed or didn't exit normally
  bool truncated = false;  // stdout passed max_out bytes, and the program was stopped
  bool timed_out = false;
  std::string out;  // stdout, at most max_out bytes
  std::string err;  // stderr, at most ProcessOptions::max_err bytes
};

// A value for a curl config file (curl -K): double-quoted, with \ and " escaped. Headers and
// credentials travel to curl that way, never on its command line, where any user on the
// machine could read them.
std::string curl_quoted(std::string_view value);

// Runs argv[0] from PATH with `input` on stdin. Stops it after timeout_ms or once stdout
// passes max_out bytes.
ProcessResult run_process(const std::vector<std::string>& argv, const std::string& input, size_t max_out,
                          int64_t timeout_ms);

struct ProcessOptions {
  std::optional<std::filesystem::path> cwd;      // where it runs; the host's own directory if unset
  std::optional<std::vector<std::string>> env;   // NAME=value entries replacing the environment
  size_t max_err = 4096;                         // how much of stderr is kept
  bool own_group = false;  // its own process group, all of which is killed when it is stopped
};

ProcessResult run_process(const std::vector<std::string>& argv, const std::string& input, size_t max_out,
                          int64_t timeout_ms, const ProcessOptions& options);

}  // namespace goober::base
