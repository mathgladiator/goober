#pragma once

// Runs a program the host trusts (curl, for fetching and for mail), with an argument
// vector and never a shell, so no value is ever parsed as a command.

#include <cstdint>
#include <string>
#include <vector>

namespace goober::base {

struct ProcessResult {
  bool started = false;    // false when the program couldn't be run at all
  int exit_code = -1;      // -1 when it was killed or didn't exit normally
  bool truncated = false;  // stdout passed max_out bytes, and the program was stopped
  bool timed_out = false;
  std::string out;  // stdout, at most max_out bytes
  std::string err;  // stderr, at most 4096 bytes
};

// Runs argv[0] from PATH with `input` on stdin. Stops it after timeout_ms or once stdout
// passes max_out bytes.
ProcessResult run_process(const std::vector<std::string>& argv, const std::string& input, size_t max_out,
                          int64_t timeout_ms);

}  // namespace goober::base
