#pragma once

// Token buckets in memory, keyed by a bucket name and a key like a client address
// (PROGRESS.md Phase 3; design/machine/013.SecurityReview.md). They slow down guessing
// and floods; they reset when the host restarts, which is fine for their job.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace goober::host {

struct RateRule {
  double per_minute = 60;  // the steady rate
  double burst = 30;       // how many may come at once
};

// The key a client's rate limits are kept under: its IPv4 address, or its IPv6 /64, since
// one person routinely holds a whole /64 and could otherwise rotate past every budget.
std::string rate_key(const std::string& address);

class RateLimiter {
 public:
  // Takes one token for (bucket, key). Returns 0 when allowed, or the seconds to wait.
  int64_t take(std::string_view bucket, std::string_view key, const RateRule& rule, int64_t now_ms);
  size_t size() const;

 private:
  struct Bucket {
    double tokens = 0;
    int64_t last_ms = 0;
    double burst = 0;
  };
  mutable std::mutex mu_;
  std::map<std::string, Bucket, std::less<>> buckets_;
  uint64_t calls_ = 0;
};

}  // namespace goober::host
