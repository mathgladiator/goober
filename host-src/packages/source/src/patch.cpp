// A revision as `git format-patch` writes it (design/machine/020.Source.md § 10), so a mirrored
// history replays into git with `git am`. Text files are unified diffs; binary files are git
// binary patches, written without zlib: a zlib stream may hold stored (uncompressed) blocks.

#include <ctime>
#include <map>
#include <set>

#include "goober/base/hash.h"
#include "goober/source/source.h"

namespace goober::source {

namespace {

constexpr const char* kNoBlob = "0000000000000000000000000000000000000000";

// zlib (RFC 1950) around stored deflate blocks (RFC 1951 § 3.2.4), with the Adler-32 check.
std::string zlib_stored(std::string_view data) {
  std::string out = "\x78\x01";
  size_t at = 0;
  do {
    size_t n = std::min<size_t>(65535, data.size() - at);
    bool last = at + n == data.size();
    out += char(last ? 1 : 0);
    out += char(n & 0xff);
    out += char(n >> 8);
    out += char(~n & 0xff);
    out += char((~n >> 8) & 0xff);
    out.append(data.substr(at, n));
    at += n;
  } while (at < data.size());
  uint32_t a = 1, b = 0;
  for (unsigned char c : data) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  uint32_t adler = (b << 16) | a;
  for (int shift = 24; shift >= 0; shift -= 8) out += char((adler >> shift) & 0xff);
  return out;
}

// Git's base85 (base85.c): its own alphabet, 4 bytes to 5 digits, most significant first.
constexpr std::string_view kBase85 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";

std::string date_of(int64_t ms) {
  std::time_t t = std::time_t(ms / 1000);
  std::tm tm{};
  gmtime_r(&t, &tm);
  static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s, %d %s %d %02d:%02d:%02d +0000", kDays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon],
                tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

// An author like token:grok or config as a mail address git accepts.
std::string address_of(const std::string& author) {
  std::string local;
  for (char c : author) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    local += ok ? c : '-';
  }
  if (local.empty()) local = "unknown";
  return local.substr(0, 64) + "@goober.invalid";
}

// A message line that a patch's reader would take for the patch itself is set off by a space.
std::string body_line(const std::string& line) {
  if (line == "---" || line.starts_with("--- ") || line.starts_with("diff -") || line.starts_with("Index: ") ||
      line.starts_with("From ")) {
    return " " + line;
  }
  return line;
}

}  // namespace

std::string git_blob_id(std::string_view content) {
  std::string object = "blob " + std::to_string(content.size());
  object += '\0';
  object.append(content);
  return base::sha1_hex(object);
}

std::string git_binary_literal(std::string_view content) {
  std::string z = zlib_stored(content);
  std::string out = "literal " + std::to_string(content.size()) + "\n";
  for (size_t at = 0; at < z.size(); at += 52) {
    size_t n = std::min<size_t>(52, z.size() - at);
    out += n <= 26 ? char('A' + n - 1) : char('a' + n - 27);
    for (size_t i = 0; i < n; i += 4) {
      uint32_t v = 0;
      for (size_t j = 0; j < 4; ++j) {
        v = (v << 8) | (i + j < n ? uint8_t(z[at + i + j]) : 0);
      }
      char digits[5];
      for (int d = 4; d >= 0; --d) {
        digits[d] = kBase85[v % 85];
        v /= 85;
      }
      out.append(digits, 5);
    }
    out += '\n';
  }
  return out + "\n";
}

std::optional<std::string> Store::patch(const std::string& name, int64_t rev, std::string* error) {
  auto r = revision(name, rev);
  if (!r) return *error = name + " has no revision " + std::to_string(rev), std::nullopt;
  std::map<std::string, std::string> before, after;  // path -> sha256
  auto tree = [&](int64_t at, std::map<std::string, std::string>* out) -> bool {
    if (at == 0) return true;
    auto files = this->files(name, at, error);
    if (!files) return false;
    for (const auto& f : *files) (*out)[f.path] = f.sha256;
    return true;
  };
  if (!tree(rev - 1, &before) || !tree(rev, &after)) return std::nullopt;

  std::string subject = r->message.substr(0, r->message.find('\n'));
  std::string body = r->message.find('\n') == std::string::npos ? "" : r->message.substr(r->message.find('\n') + 1);
  std::string out = "From " + std::string(kNoBlob) + " Mon Sep 17 00:00:00 2001\n";
  // The name quoted, as RFC 5322 wants for a colon or an @ in it.
  std::string quoted = "\"";
  for (char c : r->author) {
    if (c == '"' || c == '\\') quoted += '\\';
    if (c != '\n' && c != '\r') quoted += c;
  }
  out += "From: " + quoted + "\" <" + address_of(r->author) + ">\n";
  out += "Date: " + date_of(r->at_ms) + "\n";
  out += "Subject: [PATCH] " + subject + "\n";
  out += "MIME-Version: 1.0\nContent-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n\n";
  size_t at = 0;
  while (at < body.size()) {
    size_t nl = body.find('\n', at);
    std::string line = body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    out += body_line(line) + "\n";
    if (nl == std::string::npos) break;
    at = nl + 1;
  }
  if (!body.empty()) out += "\n";
  out += "Goober-Project: " + name + "\nGoober-Revision: " + std::to_string(rev) + "\n---\n\n";

  std::set<std::string> paths;
  for (const auto& [p, _] : before) paths.insert(p);
  for (const auto& [p, _] : after) paths.insert(p);
  for (const auto& path : paths) {
    auto ib = before.find(path), ia = after.find(path);
    if (ib != before.end() && ia != after.end() && ib->second == ia->second) continue;
    std::string old_bytes, new_bytes;
    if (ib != before.end()) old_bytes = read(name, rev - 1, path, error).value_or("");
    if (ia != after.end()) new_bytes = read(name, rev, path, error).value_or("");
    bool added = ib == before.end(), deleted = ia == after.end();
    std::string old_id = added ? kNoBlob : git_blob_id(old_bytes);
    std::string new_id = deleted ? kNoBlob : git_blob_id(new_bytes);
    out += "diff --git a/" + path + " b/" + path + "\n";
    if (added) out += "new file mode 100644\n";
    if (deleted) out += "deleted file mode 100644\n";
    out += "index " + old_id + ".." + new_id + (added || deleted ? "" : " 100644") + "\n";
    if (!is_text(old_bytes) || !is_text(new_bytes)) {
      out += "GIT binary patch\n" + git_binary_literal(new_bytes) + git_binary_literal(old_bytes);
      continue;
    }
    out += unified_diff(old_bytes, new_bytes, added ? "/dev/null" : "a/" + path, deleted ? "/dev/null" : "b/" + path);
  }
  out += "-- \ngoober\n\n";
  return out;
}

}  // namespace goober::source
