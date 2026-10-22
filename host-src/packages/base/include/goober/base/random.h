#pragma once

#include <cstdint>
#include <span>

namespace goober::base {

// 64 bits from the operating system's secure generator.
uint64_t os_random_u64();

// Fills the span from the operating system's secure generator.
void os_random_fill(std::span<uint8_t> out);

// xoshiro256** seeded by splitmix64. Deterministic for a seed, so a request's
// randomness can be replayed from the seed in its access log. Not for secrets.
class SeededRandom {
 public:
  explicit SeededRandom(uint64_t seed);
  uint64_t next();
  void fill(std::span<uint8_t> out);

 private:
  uint64_t s_[4];
};

}  // namespace goober::base
