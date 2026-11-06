#include "mailbox.h"

#include <algorithm>
#include <fstream>
#include <vector>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/random.h"
#include "goober/json/json.h"

namespace goober::host::mailbox {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

// Message ids in the directory, newest first.
std::vector<std::string> ids_of(const fs::path& dir) {
  std::vector<std::string> ids;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::string n = e.path().filename().string();
    if (n.ends_with(".json") && valid_id(n.substr(0, n.size() - 5))) ids.push_back(n.substr(0, n.size() - 5));
  }
  std::sort(ids.rbegin(), ids.rend());
  return ids;
}

int64_t at_of(const std::string& id) { return std::strtoll(id.substr(0, 13).c_str(), nullptr, 10); }

}  // namespace

std::string new_id(int64_t now_ms) {
  std::string at = std::to_string(now_ms);
  uint8_t raw[4];
  base::os_random_fill(raw);
  return std::string(13 - std::min<size_t>(13, at.size()), '0') + at + "-" +
         base::hex_encode(std::string_view(reinterpret_cast<const char*>(raw), sizeof raw));
}

bool valid_id(std::string_view id) {
  if (id.size() != 22 || id[13] != '-') return false;
  for (size_t i = 0; i < id.size(); ++i) {
    char c = id[i];
    if (i == 13) continue;
    if (i < 13 ? !(c >= '0' && c <= '9') : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string cut_utf8(std::string_view s, size_t max) {
  if (s.size() <= max) return std::string(s);
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return std::string(s.substr(0, n));
}

bool write(const fs::path& dir, const std::string& id, const bj::object& message, const bj::object& head) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (!base::write_file_durable(dir / (id + ".json"), json::dump(message))) return false;
  // The row is rebuilt from the message if lost, so it needn't be durable.
  // A torn one doesn't parse, and the list reads the message instead.
  std::ofstream out(dir / (id + ".head"), std::ios::binary | std::ios::trunc);
  out << json::dump(head);
  return true;
}

void trim(const fs::path& dir, size_t max_count, int64_t max_age_ms, int64_t budget, int64_t now_ms) {
  auto ids = ids_of(dir);
  int64_t total = 0;
  for (size_t i = 0; i < ids.size(); ++i) {
    std::error_code se;
    auto size = fs::file_size(dir / (ids[i] + ".json"), se);
    total += se ? 0 : int64_t(size);
    bool old = at_of(ids[i]) < now_ms - max_age_ms;
    // The newest message stays whatever its size.
    if (i > 0 && (old || i >= max_count || total > budget)) remove(dir, ids[i]);
  }
}

bj::array list(const fs::path& dir, int64_t limit, const HeadOf& head_of, const Keep& keep) {
  bj::array out;
  for (const auto& id : ids_of(dir)) {
    if (int64_t(out.size()) >= limit) break;
    std::string err;
    std::optional<bj::object> row;
    if (auto head = base::read_file(dir / (id + ".head"))) {
      if (auto h = json::parse(*head, &err); h && h->is_object()) row = std::move(h->as_object());
    }
    if (!row) {
      auto m = get(dir, id);
      if (!m) continue;
      row = head_of(*m);
    }
    if (keep && !keep(*row)) continue;
    out.push_back(std::move(*row));
  }
  return out;
}

std::optional<bj::object> get(const fs::path& dir, const std::string& id) {
  if (!valid_id(id)) return std::nullopt;
  auto text = base::read_file(dir / (id + ".json"));
  std::string err;
  auto v = text ? json::parse(*text, &err) : std::nullopt;
  if (!v || !v->is_object()) return std::nullopt;
  return std::move(v->as_object());
}

bool remove(const fs::path& dir, const std::string& id) {
  if (!valid_id(id)) return false;
  std::error_code ec;
  bool was = fs::remove(dir / (id + ".json"), ec);
  fs::remove(dir / (id + ".head"), ec);
  fs::remove_all(dir / (id + ".parts"), ec);
  return was;
}

int64_t remove_before(const fs::path& dir, int64_t before_ms) {
  int64_t n = 0;
  for (const auto& id : ids_of(dir)) {
    if (before_ms > 0 && at_of(id) >= before_ms) continue;
    n += remove(dir, id) ? 1 : 0;
  }
  return n;
}

int64_t bytes(const fs::path& dir) {
  int64_t n = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) n += int64_t(it->file_size(e2));
  }
  return n;
}

}  // namespace goober::host::mailbox
