#include "goober/host/rate_limit.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cmath>

#include "goober/base/encoding.h"

namespace goober::host {

std::string rate_key(const std::string& address) {
  in6_addr a6{};
  if (address.find(':') != std::string::npos && inet_pton(AF_INET6, address.c_str(), &a6) == 1) {
    // An IPv4 client on a dual-stack listener arrives as ::ffff:a.b.c.d: it is its IPv4
    // address, not the /64 every IPv4 client shares.
    if (IN6_IS_ADDR_V4MAPPED(&a6)) {
      char v4[INET_ADDRSTRLEN] = {};
      if (inet_ntop(AF_INET, a6.s6_addr + 12, v4, sizeof v4)) return v4;
    }
    return "v6:" + base::hex_encode(std::string_view(reinterpret_cast<const char*>(a6.s6_addr), 8));
  }
  return address;
}

int64_t RateLimiter::take(std::string_view bucket, std::string_view key, const RateRule& rule, int64_t now_ms) {
  std::string id = std::string(bucket) + "\n" + std::string(key);
  std::lock_guard lock(mu_);
  // Now and then, forget buckets that have refilled: they hold nothing worth keeping.
  if (++calls_ % 4096 == 0) {
    for (auto it = buckets_.begin(); it != buckets_.end();) {
      bool idle = now_ms - it->second.last_ms > 15 * 60 * 1000;
      it = idle ? buckets_.erase(it) : std::next(it);
    }
  }
  auto [it, fresh] = buckets_.try_emplace(id, Bucket{rule.burst, now_ms, rule.burst});
  Bucket& b = it->second;
  if (!fresh) {
    double elapsed = double(std::max<int64_t>(0, now_ms - b.last_ms));
    b.tokens = std::min(rule.burst, b.tokens + elapsed * rule.per_minute / 60000.0);
    b.last_ms = now_ms;
  }
  if (b.tokens >= 1) {
    b.tokens -= 1;
    return 0;
  }
  double wait_ms = (1 - b.tokens) * 60000.0 / rule.per_minute;
  return std::max<int64_t>(1, int64_t(std::ceil(wait_ms / 1000.0)));
}

size_t RateLimiter::size() const {
  std::lock_guard lock(mu_);
  return buckets_.size();
}

}  // namespace goober::host
