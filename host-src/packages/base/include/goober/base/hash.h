#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace goober::base {

// SHA-256 of the bytes, as 32 raw bytes.
std::array<uint8_t, 32> sha256(std::string_view data);

// SHA-256 of the bytes, as 64 lowercase hex digits.
std::string sha256_hex(std::string_view data);

// CRC-32 (IEEE 802.3, the zlib polynomial) of the bytes.
uint32_t crc32(std::string_view data);

}  // namespace goober::base
