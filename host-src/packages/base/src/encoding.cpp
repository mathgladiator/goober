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

std::string base64url_encode(std::string_view bytes) {
  std::string out = base64_encode(bytes);
  while (!out.empty() && out.back() == '=') out.pop_back();
  for (char& c : out) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
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

std::string percent_encode(std::string_view text) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                c == '_' || c == '~';
    if (keep) {
      out += char(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

std::string html_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
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

std::string valid_utf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    bool ok = n > 0 && i + size_t(n) <= s.size();
    for (int k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + size_t(k)]) & 0xC0) == 0x80;
    if (ok && n == 2) ok = c >= 0xC2;
    if (ok && n == 3) {
      unsigned char c1 = s[i + 1];
      ok = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0);
    }
    if (ok && n == 4) {
      unsigned char c1 = s[i + 1];
      ok = c <= 0xF4 && !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
    }
    if (!ok) {
      out += "\xEF\xBF\xBD";
      ++i;
      continue;
    }
    out.append(s.substr(i, size_t(n)));
    i += size_t(n);
  }
  return out;
}

std::optional<std::string> base64url_decode(std::string_view text) {
  std::string s;
  s.reserve(text.size() + 3);
  for (char c : text) {
    if (c == '+' || c == '/') return std::nullopt;  // the other alphabet
    s += c == '-' ? '+' : c == '_' ? '/' : c;
  }
  while (!s.empty() && s.back() == '=') s.pop_back();
  if (s.size() % 4 == 1) return std::nullopt;
  while (s.size() % 4) s += '=';
  return base64_decode(s);
}

}  // namespace goober::base
