#include "goober/base/chacha20.h"

namespace goober::base {
namespace {

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t load_le(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void quarter(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
  a += b, d ^= a, d = rotl(d, 16);
  c += d, b ^= c, b = rotl(b, 12);
  a += b, d ^= a, d = rotl(d, 8);
  c += d, b ^= c, b = rotl(b, 7);
}

}  // namespace

ChaCha20::ChaCha20(const std::array<uint8_t, 32>& key, const std::array<uint8_t, 12>& nonce, uint32_t counter) {
  input_[0] = 0x61707865;
  input_[1] = 0x3320646e;
  input_[2] = 0x79622d32;
  input_[3] = 0x6b206574;
  for (int i = 0; i < 8; ++i) input_[4 + i] = load_le(key.data() + 4 * i);
  input_[12] = counter;
  for (int i = 0; i < 3; ++i) input_[13 + i] = load_le(nonce.data() + 4 * i);
}

void ChaCha20::next_block() {
  uint32_t x[16];
  for (int i = 0; i < 16; ++i) x[i] = input_[i];
  for (int round = 0; round < 10; ++round) {
    quarter(x[0], x[4], x[8], x[12]);
    quarter(x[1], x[5], x[9], x[13]);
    quarter(x[2], x[6], x[10], x[14]);
    quarter(x[3], x[7], x[11], x[15]);
    quarter(x[0], x[5], x[10], x[15]);
    quarter(x[1], x[6], x[11], x[12]);
    quarter(x[2], x[7], x[8], x[13]);
    quarter(x[3], x[4], x[9], x[14]);
  }
  for (int i = 0; i < 16; ++i) {
    uint32_t v = x[i] + input_[i];
    block_[4 * i] = uint8_t(v);
    block_[4 * i + 1] = uint8_t(v >> 8);
    block_[4 * i + 2] = uint8_t(v >> 16);
    block_[4 * i + 3] = uint8_t(v >> 24);
  }
  ++input_[12];
  used_ = 0;
}

void ChaCha20::apply(uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (used_ == 64) next_block();
    data[i] ^= block_[used_++];
  }
}

}  // namespace goober::base
