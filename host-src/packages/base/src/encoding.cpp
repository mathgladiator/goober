#include "goober/base/encoding.h"

#include <cctype>
#include <cstdint>

namespace goober::base {
namespace {
constexpr char kHex[] = "0123456789abcdef";
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

std::string hex_encode(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0xF]);
  }
  return out;
}

std::string base64_encode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    uint32_t n = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
    out.push_back(kBase64[(n >> 18) & 63]);
    out.push_back(kBase64[(n >> 12) & 63]);
    out.push_back(kBase64[(n >> 6) & 63]);
    out.push_back(kBase64[n & 63]);
  }
  if (i + 1 == bytes.size()) {
    uint32_t n = uint8_t(bytes[i]) << 16;
    out.push_back(kBase64[(n >> 18) & 63]);
    out.push_back(kBase64[(n >> 12) & 63]);
    out += "==";
  } else if (i + 2 == bytes.size()) {
    uint32_t n = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8);
    out.push_back(kBase64[(n >> 18) & 63]);
    out.push_back(kBase64[(n >> 12) & 63]);
    out.push_back(kBase64[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::optional<std::string> base64_decode(std::string_view text) {
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  bool padding = false;
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    if (c == '=') {
      padding = true;
      continue;
    }
    if (padding) return std::nullopt;
    int v = base64_value(c);
    if (v < 0) return std::nullopt;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(char((acc >> bits) & 0xFF));
    }
  }
  return out;
}

std::optional<std::string> percent_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size()) return std::nullopt;
    int hi = hex_value(text[i + 1]);
    int lo = hex_value(text[i + 2]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(char(hi * 16 + lo));
    i += 2;
  }
  return out;
}

std::string to_lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace goober::base
