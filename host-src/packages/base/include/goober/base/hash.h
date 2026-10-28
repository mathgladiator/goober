#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace goober::base {

// SHA-256 over bytes given in pieces, for files too large to hold at once. finish() returns
// the digest and starts over.
class Sha256 {
 public:
  Sha256();
  void update(std::string_view data);
  std::array<uint8_t, 32> finish();

 private:
  void reset();
  uint32_t state_[8];
  uint8_t buffer_[64];
  size_t buffered_ = 0;
  uint64_t total_ = 0;
};

// SHA-256 of the bytes, as 32 raw bytes.
std::array<uint8_t, 32> sha256(std::string_view data);

// SHA-256 of the bytes, as 64 lowercase hex digits.
std::string sha256_hex(std::string_view data);

// SHA-256 of a file's bytes, read in pieces, as 64 lowercase hex digits; nullopt when it
// can't be read.
std::optional<std::string> sha256_file_hex(const std::filesystem::path& path);

// HMAC-SHA-256 (RFC 2104) of the data under the key, as 32 raw bytes or 64 hex digits.
std::array<uint8_t, 32> hmac_sha256(std::string_view key, std::string_view data);
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

// True when a and b hold the same bytes, taking the same time wherever they differ.
bool equal_constant_time(std::string_view a, std::string_view b);

// CRC-32 (IEEE 802.3, the zlib polynomial) of the bytes.
uint32_t crc32(std::string_view data);

}  // namespace goober::base
