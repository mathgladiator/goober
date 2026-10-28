#pragma once

// ChaCha20 (RFC 8439 §2.4): a stream cipher with a 256-bit key and a 96-bit nonce. It hides
// bytes but doesn't authenticate them; whoever uses it pairs it with a MAC (the backups seal
// files with ChaCha20 and HMAC-SHA-256, encrypt-then-MAC).

#include <array>
#include <cstddef>
#include <cstdint>

namespace goober::base {

class ChaCha20 {
 public:
  ChaCha20(const std::array<uint8_t, 32>& key, const std::array<uint8_t, 12>& nonce, uint32_t counter = 0);
  // XORs the next n bytes of the key stream into data. Calls may split the stream anywhere.
  void apply(uint8_t* data, size_t n);

 private:
  void next_block();
  uint32_t input_[16];
  uint8_t block_[64];
  size_t used_ = 64;
};

}  // namespace goober::base
