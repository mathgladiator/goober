#pragma once

// The host's pulse (design/machine/028.Panel.md § 4.6): counters and request latency for the
// last five minutes, in memory, for /goober/ready's health numbers. Minute buckets in a ring, so
// recording is a lock and an add and reading never touches a log file. Lost on restart.

#include <array>
#include <cstdint>
#include <mutex>

#include "goober/operations/traffic.h"

namespace goober::operations {

class Pulse {
 public:
  static constexpr int kMinutes = 5;
  static constexpr int64_t kMinuteMs = 60 * 1000;
  enum Counter {
    kRequests,
    kRequests4xx,
    kRequests5xx,
    kBoxCalls,
    kBoxFailed,
    kMailSent,
    kMailFailed,
    kServiceCalls,
    kServiceFailed,
    kCounters
  };

  void add(int64_t now_ms, Counter c, int64_t n = 1);
  // One request answered: counted, by status class, and its latency kept.
  void request(int64_t now_ms, int status, int64_t us);

  struct Snapshot {
    std::array<int64_t, kCounters> n{};
    int64_t p50_us = 0, p99_us = 0, max_us = 0;
  };
  // The current minute and the four before it.
  Snapshot snapshot(int64_t now_ms) const;

 private:
  struct Minute {
    int64_t at = -1;  // now_ms / kMinuteMs, or -1
    std::array<int64_t, kCounters> n{};
    Histogram hist;
    int64_t max_us = 0;
  };
  Minute& minute(int64_t now_ms);  // with mu_ held
  mutable std::mutex mu_;
  std::array<Minute, kMinutes> ring_{};
};

}  // namespace goober::operations
