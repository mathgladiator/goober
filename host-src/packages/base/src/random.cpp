#include "goober/base/random.h"

#include <sys/random.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace goober::base {
namespace {
uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
}  // namespace

void os_random_fill(std::span<uint8_t> out) {
  size_t done = 0;
  while (done < out.size()) {
    ssize_t n = getrandom(out.data() + done, out.size() - done, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("getrandom failed: ") + std::strerror(errno));
    }
    done += size_t(n);
  }
}

uint64_t os_random_u64() {
  uint64_t v = 0;
  os_random_fill(std::span<uint8_t>(reinterpret_cast<uint8_t*>(&v), sizeof(v)));
  return v;
}

SeededRandom::SeededRandom(uint64_t seed) {
  for (auto& s : s_) s = splitmix64(seed);
}

uint64_t SeededRandom::next() {
  uint64_t result = rotl(s_[1] * 5, 7) * 9;
  uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return result;
}

void SeededRandom::fill(std::span<uint8_t> out) {
  size_t i = 0;
  while (i < out.size()) {
    uint64_t v = next();
    for (int k = 0; k < 8 && i < out.size(); ++k, ++i) out[i] = uint8_t(v >> (8 * k));
  }
}

}  // namespace goober::base
