// Sealed files: ChaCha20 with HMAC-SHA-256, encrypt-then-MAC, keys per file from a salt.

#include <cstring>
#include <fstream>

#include "goober/backup/backup.h"
#include "goober/base/chacha20.h"
#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"

namespace goober::backup {
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kMagic = "GOOBERS1";
constexpr size_t kSalt = 16, kTag = 32, kChunk = 1 << 16;

std::string_view bytes(const std::array<uint8_t, 32>& a) {
  return std::string_view(reinterpret_cast<const char*>(a.data()), a.size());
}

// The file's own keys: one to encrypt, one to authenticate.
void derive(const Key& key, std::string_view salt, Key* enc, Key* mac) {
  *enc = base::hmac_sha256(bytes(key), "goober-backup-enc\n" + std::string(salt));
  *mac = base::hmac_sha256(bytes(key), "goober-backup-mac\n" + std::string(salt));
}

}  // namespace

std::optional<Key> parse_key(std::string_view hex) {
  while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) hex.remove_suffix(1);
  if (hex.size() != 64) return std::nullopt;
  Key k{};
  for (size_t i = 0; i < 32; ++i) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    k[i] = uint8_t(hi * 16 + lo);
  }
  return k;
}

std::string key_hex(const Key& key) { return base::hex_encode(bytes(key)); }

Key new_key() {
  Key k{};
  base::os_random_fill(k);
  return k;
}

bool seal_file(const Key& key, const fs::path& in_path, const fs::path& out_path, std::string* error) {
  std::ifstream in(in_path, std::ios::binary);
  if (!in) return *error = "can't read " + in_path.string(), false;
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) return *error = "can't write " + out_path.string(), false;
  std::array<uint8_t, kSalt> salt_bytes{};
  base::os_random_fill(salt_bytes);
  std::string salt(reinterpret_cast<char*>(salt_bytes.data()), kSalt);
  Key enc, mac_key;
  derive(key, salt, &enc, &mac_key);
  base::HmacSha256 mac(bytes(mac_key));
  base::ChaCha20 cipher(enc, std::array<uint8_t, 12>{});
  out.write(kMagic.data(), kMagic.size());
  out.write(salt.data(), salt.size());
  mac.update(kMagic);
  mac.update(salt);
  std::string buf(kChunk, '\0');
  while (in) {
    in.read(buf.data(), std::streamsize(buf.size()));
    size_t n = size_t(in.gcount());
    if (n == 0) break;
    cipher.apply(reinterpret_cast<uint8_t*>(buf.data()), n);
    mac.update(std::string_view(buf.data(), n));
    out.write(buf.data(), std::streamsize(n));
  }
  if (in.bad()) return *error = "reading " + in_path.string() + " failed", false;
  auto tag = mac.finish();
  out.write(reinterpret_cast<const char*>(tag.data()), tag.size());
  out.flush();
  if (!out) return *error = "writing " + out_path.string() + " failed", false;
  return true;
}

bool unseal_file(const Key& key, const fs::path& in_path, const fs::path& out_path, std::string* error) {
  std::error_code ec;
  auto size = fs::file_size(in_path, ec);
  if (ec) return *error = "can't read " + in_path.string(), false;
  if (size < kMagic.size() + kSalt + kTag) return *error = in_path.filename().string() + " is too short to be sealed", false;
  uint64_t body = size - kMagic.size() - kSalt - kTag;
  std::ifstream in(in_path, std::ios::binary);
  std::string head(kMagic.size() + kSalt, '\0');
  in.read(head.data(), std::streamsize(head.size()));
  if (!in || std::string_view(head).substr(0, kMagic.size()) != kMagic) {
    return *error = in_path.filename().string() + " isn't a sealed goober backup", false;
  }
  std::string salt = head.substr(kMagic.size());
  Key enc, mac_key;
  derive(key, salt, &enc, &mac_key);
  // First pass: the tag. Nothing is decrypted until the whole file is known to be authentic.
  base::HmacSha256 mac(bytes(mac_key));
  mac.update(head);
  std::string buf(kChunk, '\0');
  for (uint64_t left = body; left > 0;) {
    size_t n = size_t(std::min<uint64_t>(left, buf.size()));
    in.read(buf.data(), std::streamsize(n));
    if (!in) return *error = "reading " + in_path.string() + " failed", false;
    mac.update(std::string_view(buf.data(), n));
    left -= n;
  }
  std::string tag(kTag, '\0');
  in.read(tag.data(), kTag);
  auto expected = mac.finish();
  if (!in || !base::equal_constant_time(tag, bytes(expected))) {
    return *error = in_path.filename().string() + " fails its check: the wrong backup key, or a damaged file", false;
  }
  // Second pass: decrypt into a temporary file, renamed into place when whole.
  in.clear();
  in.seekg(std::streamoff(head.size()));
  fs::path tmp = out_path.string() + ".unsealing";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) return *error = "can't write " + tmp.string(), false;
  base::ChaCha20 cipher(enc, std::array<uint8_t, 12>{});
  for (uint64_t left = body; left > 0;) {
    size_t n = size_t(std::min<uint64_t>(left, buf.size()));
    in.read(buf.data(), std::streamsize(n));
    if (!in) return *error = "reading " + in_path.string() + " failed", false;
    cipher.apply(reinterpret_cast<uint8_t*>(buf.data()), n);
    out.write(buf.data(), std::streamsize(n));
    left -= n;
  }
  out.close();
  if (!out) {
    fs::remove(tmp, ec);
    return *error = "writing " + out_path.string() + " failed", false;
  }
  fs::rename(tmp, out_path, ec);
  if (ec) return *error = "can't move " + out_path.string() + " into place: " + ec.message(), false;
  return true;
}

}  // namespace goober::backup
