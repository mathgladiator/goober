#pragma once

#include <cstdint>
#include <string>

namespace goober::base {

// Milliseconds since the Unix epoch, from the system clock.
int64_t now_unix_ms();

// A monotonic clock in microseconds, for measuring durations.
int64_t monotonic_us();

// RFC 3339 UTC, like 2026-10-22T03:04:05.678Z.
std::string format_utc_ms(int64_t unix_ms);

}  // namespace goober::base
