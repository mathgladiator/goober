#include "goober/logs/logs.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <fstream>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/time.h"

namespace goober::logs {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kChainKey = ",\"chain\":\"";
constexpr size_t kChainSuffix = 10 + 32 + 2;  // ,"chain":"  32 hex  "}

bool write_all(int fd, std::string_view data) {
  while (!data.empty()) {
    ssize_t n = ::write(fd, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data.remove_prefix(size_t(n));
  }
  return true;
}

// The last complete line of a file, read from its end.
std::string last_line(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return "";
  in.seekg(0, std::ios::end);
  int64_t size = in.tellg();
  int64_t chunk = 65536;
  std::string tail;
  for (int64_t end = size; end > 0 && tail.find('\n', 0) == std::string::npos;) {
    int64_t start = std::max<int64_t>(0, end - chunk);
    std::string piece(size_t(end - start), '\0');
    in.seekg(start);
    in.read(piece.data(), end - start);
    tail = piece + tail;
    end = start;
    // Need a newline before the last line (or the file start).
    std::string_view t(tail);
    if (t.size() >= 2) {
      size_t nl = t.substr(0, t.size() - 1).rfind('\n');
      if (nl != std::string_view::npos || start == 0) break;
    } else if (start == 0) {
      break;
    }
  }
  while (!tail.empty() && tail.back() == '\n') tail.pop_back();
  size_t nl = tail.rfind('\n');
  return nl == std::string::npos ? tail : tail.substr(nl + 1);
}

std::string first_line(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::string line;
  std::getline(in, line);
  return line;
}

int64_t seq_of(std::string_view line) {
  std::string err;
  auto v = json::parse(line, &err);
  if (!v || !v->is_object()) return 0;
  auto* s = v->as_object().if_contains("seq");
  return s && s->is_int64() ? s->as_int64() : 0;
}

std::string day_of(int64_t ms) { return base::format_utc_ms(ms).substr(0, 10); }

std::string file_safe(std::string name) {
  for (auto& c : name) {
    if (c == '/') c = '_';
  }
  return name;
}

}  // namespace

std::string chain_of(std::string_view prev, std::string_view body) {
  base::Sha256 h;
  h.update(prev);
  h.update("\n");
  h.update(body);
  auto d = h.finish();
  return base::hex_encode(std::string_view(reinterpret_cast<const char*>(d.data()), 16));
}

std::string chained_line(std::string_view body, std::string_view chain) {
  std::string out(body.substr(0, body.size() - 1));
  out += kChainKey;
  out += chain;
  out += "\"}";
  return out;
}

std::optional<std::pair<std::string, std::string>> split_chain(std::string_view line) {
  if (line.size() < kChainSuffix + 2 || line.substr(line.size() - 2) != "\"}") return std::nullopt;
  size_t at = line.size() - kChainSuffix;
  if (line.substr(at, kChainKey.size()) != kChainKey) return std::nullopt;
  std::string chain(line.substr(at + kChainKey.size(), 32));
  for (char c : chain) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
  }
  std::string body(line.substr(0, at));
  body += "}";
  return std::pair{std::move(body), std::move(chain)};
}

std::optional<int64_t> parse_at(std::string_view at) {
  // YYYY-MM-DDTHH:MM:SS(.mmm)Z
  if (at.size() < 20 || at[4] != '-' || at[7] != '-' || at[10] != 'T' || at[13] != ':' || at[16] != ':') return std::nullopt;
  auto num = [&](size_t i, size_t n) {
    int64_t v = 0;
    for (size_t k = i; k < i + n; ++k) {
      if (at[k] < '0' || at[k] > '9') return int64_t(-1);
      v = v * 10 + (at[k] - '0');
    }
    return v;
  };
  int64_t y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2), s = num(17, 2);
  if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || mi < 0 || s < 0) return std::nullopt;
  int64_t ms = 0;
  if (at.size() >= 24 && at[19] == '.') ms = std::max<int64_t>(0, num(20, 3));
  // Days from civil (Howard Hinnant).
  int64_t yy = mo <= 2 ? y - 1 : y;
  int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  int64_t yoe = yy - era * 400;
  int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  return ((days * 24 + h) * 60 + mi) * 60000 + s * 1000 + ms;
}

bool valid_name(std::string_view name) {
  auto plain = [](std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
  };
  if (name.starts_with("boxes/")) return plain(name.substr(6));
  return plain(name) && name.find('-') == std::string_view::npos;
}

Writer::Writer(fs::path file, std::string name, std::string host, uint64_t max_bytes)
    : file_(std::move(file)), name_(std::move(name)), host_(std::move(host)), max_bytes_(max_bytes) {}

std::optional<Rotated> Writer::rotate_locked(int fd, int64_t now_ms) {
  // The manifest: what this file holds, so its archive can be checked on its own.
  std::ifstream in(file_, std::ios::binary);
  base::Sha256 sha;
  std::string line, first_prev, last_chain, first_seq_text;
  int64_t lines = 0, first_seq = 0, last_seq = 0;
  std::string pending;
  char buf[65536];
  while (in.read(buf, sizeof buf) || in.gcount() > 0) {
    std::string_view piece(buf, size_t(in.gcount()));
    sha.update(piece);
    pending.append(piece);
    size_t start = 0;
    for (size_t nl = pending.find('\n'); nl != std::string::npos; nl = pending.find('\n', start)) {
      std::string_view l(pending.data() + start, nl - start);
      start = nl + 1;
      if (l.empty()) continue;
      ++lines;
      int64_t s = seq_of(l);
      if (lines == 1) first_seq = s;
      last_seq = s;
      if (auto sc = split_chain(l)) last_chain = sc->second;
    }
    pending.erase(0, start);
  }
  auto digest = sha.finish();
  (void)fd;
  // Shift the numbered files up, dropping what's past kKeepFiles.
  std::error_code ec;
  fs::remove(file_.string() + "." + std::to_string(kKeepFiles), ec);
  for (int i = kKeepFiles - 1; i >= 1; --i) {
    fs::path from = file_.string() + "." + std::to_string(i);
    if (fs::exists(from, ec)) fs::rename(from, file_.string() + "." + std::to_string(i + 1), ec);
  }
  fs::path rotated = file_.string() + ".1";
  fs::rename(file_, rotated, ec);
  if (ec) return std::nullopt;
  // Queued for upload: a hard link (no copy), and the manifest beside it.
  fs::path archive = file_.parent_path() / "archive";
  if (name_.starts_with("boxes/")) archive = file_.parent_path().parent_path() / "archive";
  fs::create_directories(archive, ec);
  std::string stamp = base::format_utc_ms(now_ms);
  for (char& c : stamp) {
    if (c == ':') c = '-';
  }
  std::string base_name = file_safe(name_) + "-" + stamp;
  Rotated r;
  r.log = name_;
  r.queued = archive / (base_name + ".log");
  r.manifest = archive / (base_name + ".manifest.json");
  fs::create_hard_link(rotated, r.queued, ec);
  if (ec) fs::copy_file(rotated, r.queued, fs::copy_options::overwrite_existing, ec);
  // The chain before this file: the line before its first, which is the previous file's last.
  fs::path older = file_.string() + ".2";
  std::string prev = kGenesis.data();
  if (fs::exists(older, ec)) {
    if (auto sc = split_chain(last_line(older))) prev = sc->second;
  }
  r.manifest_json = bj::object{{"log", name_},
                               {"file", base_name + ".log"},
                               {"host", host_},
                               {"rotated_at", base::format_utc_ms(now_ms)},
                               {"first_seq", first_seq},
                               {"last_seq", last_seq},
                               {"chain_before", prev},
                               {"chain_after", last_chain},
                               {"lines", lines},
                               {"sha256", base::hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()))}};
  std::ofstream(r.manifest) << json::pretty(r.manifest_json);
  return r;
}

std::optional<Rotated> Writer::write(const bj::object& fields, int64_t now_ms, bool* ok) {
  std::lock_guard lock(mu_);
  if (ok) *ok = false;
  std::error_code ec;
  fs::create_directories(file_.parent_path(), ec);
  std::optional<Rotated> rotated;
  for (int attempt = 0; attempt < 5; ++attempt) {
    int fd = ::open(file_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return rotated;
    if (::flock(fd, LOCK_EX) != 0) {
      ::close(fd);
      return rotated;
    }
    struct stat mine {}, now {};
    // Rotated by someone else between our open and our lock: write to the new file instead.
    if (::fstat(fd, &mine) != 0 || ::stat(file_.c_str(), &now) != 0 || mine.st_ino != now.st_ino) {
      ::close(fd);
      continue;
    }
    // The tail and the file's day: cached, and read again from the file when another process
    // (or a rotation) changed it since this writer last wrote.
    if (uint64_t(mine.st_ino) != seen_ino_ || int64_t(mine.st_size) != seen_size_) {
      std::string prev_line = mine.st_size > 0 ? last_line(file_) : "";
      if (prev_line.empty()) prev_line = last_line(file_.string() + ".1");
      seen_chain_ = std::string(kGenesis);
      seen_seq_ = 0;
      if (!prev_line.empty()) {
        if (auto sc = split_chain(prev_line)) seen_chain_ = sc->second;
        seen_seq_ = seq_of(prev_line);
      }
      seen_day_.clear();
      if (mine.st_size > 0) {
        std::string err;
        auto v = json::parse(first_line(file_), &err);
        if (v && v->is_object()) {
          if (auto* a = v->as_object().if_contains("at"); a && a->is_string()) seen_day_ = std::string(a->as_string()).substr(0, 10);
        }
      }
    }
    // A file from an earlier UTC day, or one this line would push past the size, rotates first.
    if (mine.st_size > 0 && ((!seen_day_.empty() && seen_day_ < day_of(now_ms)) || uint64_t(mine.st_size) + 512 > max_bytes_)) {
      rotated = rotate_locked(fd, now_ms);
      seen_size_ = -1;
      ::close(fd);
      continue;  // write into the new file
    }
    std::string prev_chain = seen_chain_;
    int64_t seq = seen_seq_ + 1;
    bj::object line{{"at", base::format_utc_ms(now_ms)}, {"log", name_}, {"seq", seq}, {"host", host_}};
    for (const auto& kv : fields) {
      if (kv.key() == "chain" || line.contains(kv.key())) continue;
      line[kv.key()] = kv.value();
    }
    std::string body = json::dump(line);
    std::string chain = chain_of(prev_chain, body);
    std::string out = chained_line(body, chain);
    out.push_back('\n');
    bool good = write_all(fd, out);
    if (good) {
      seen_ino_ = uint64_t(mine.st_ino);
      seen_size_ = int64_t(mine.st_size) + int64_t(out.size());
      seen_chain_ = chain;
      seen_seq_ = seq;
      if (seen_day_.empty()) seen_day_ = day_of(now_ms);
    } else {
      seen_size_ = -1;
    }
    ::close(fd);
    if (ok) *ok = good;
    return rotated;
  }
  return rotated;
}

std::optional<Rotated> Writer::rotate_if_stale(int64_t now_ms) {
  std::lock_guard lock(mu_);
  int fd = ::open(file_.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  if (::flock(fd, LOCK_EX) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  struct stat st {};
  std::optional<Rotated> out;
  if (::fstat(fd, &st) == 0 && st.st_size > 0) {
    std::string err;
    auto v = json::parse(first_line(file_), &err);
    std::string at;
    if (v && v->is_object()) {
      if (auto* a = v->as_object().if_contains("at"); a && a->is_string()) at = std::string(a->as_string());
    }
    if (!at.empty() && at.substr(0, 10) < day_of(now_ms)) out = rotate_locked(fd, now_ms);
  }
  ::close(fd);
  return out;
}

Set::Set(fs::path dir, std::string host, uint64_t max_bytes) : dir_(std::move(dir)), host_(std::move(host)), max_bytes_(max_bytes) {}

Writer& Set::writer(const std::string& name) {
  std::lock_guard lock(mu_);
  auto it = writers_.find(name);
  if (it != writers_.end()) return *it->second;
  auto w = std::make_unique<Writer>(dir_ / (name + ".log"), name, host_, max_bytes_);
  auto& ref = *w;
  writers_.emplace(name, std::move(w));
  return ref;
}

std::optional<Rotated> Set::write(const std::string& name, const bj::object& fields, int64_t now_ms) {
  if (!valid_name(name)) return std::nullopt;
  return writer(name).write(fields, now_ms);
}

std::vector<std::string> Set::names() const {
  std::vector<std::string> out;
  std::error_code ec;
  auto scan = [&](const fs::path& d, const std::string& prefix) {
    for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file()) continue;
      std::string f = it->path().filename().string();
      auto at = f.find(".log");
      if (at == std::string::npos || at == 0) continue;
      std::string rest = f.substr(at + 4);
      bool numbered = rest.empty() || (rest[0] == '.' && rest.size() > 1 &&
                                       std::all_of(rest.begin() + 1, rest.end(), [](char c) { return c >= '0' && c <= '9'; }));
      if (!numbered) continue;
      std::string name = prefix + f.substr(0, at);
      if (valid_name(name) && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
  };
  scan(dir_, "");
  scan(dir_ / "boxes", "boxes/");
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<Rotated> Set::rotate_stale(int64_t now_ms) {
  std::vector<Rotated> out;
  for (const auto& name : names()) {
    if (auto r = writer(name).rotate_if_stale(now_ms)) out.push_back(std::move(*r));
  }
  return out;
}

std::vector<fs::path> files_of(const fs::path& dir, const std::string& name) {
  std::vector<fs::path> out;
  fs::path current = dir / (name + ".log");
  std::error_code ec;
  for (int i = kKeepFiles; i >= 1; --i) {
    fs::path f = current.string() + "." + std::to_string(i);
    if (fs::exists(f, ec)) out.push_back(f);
  }
  if (fs::exists(current, ec)) out.push_back(current);
  return out;
}

Verified verify(const fs::path& dir, const std::string& name) {
  Verified v;
  std::string prev;
  bool started = false;
  for (const auto& file : files_of(dir, name)) {
    ++v.files;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    int64_t n = 0;
    while (std::getline(in, line)) {
      ++n;
      if (line.empty()) continue;
      ++v.lines;
      auto sc = split_chain(line);
      if (!sc) {
        if (started) {
          v.ok = false;
          v.broken = file.filename().string() + ":" + std::to_string(n) + ": a line without a chain after chained lines";
          return v;
        }
        ++v.unchained;
        continue;
      }
      // The first chained line in the local files chains from what came before them: from
      // genesis, or from a file no longer here (its manifest says).
      if (started && chain_of(prev, sc->first) != sc->second) {
        v.ok = false;
        v.broken = file.filename().string() + ":" + std::to_string(n) + ": the chain doesn't follow (seq " +
                   std::to_string(seq_of(line)) + ")";
        return v;
      }
      if (!started && seq_of(line) == 1 && chain_of(kGenesis, sc->first) != sc->second) {
        v.ok = false;
        v.broken = file.filename().string() + ":" + std::to_string(n) + ": the first line doesn't chain from genesis";
        return v;
      }
      if (!started) v.first_seq = std::to_string(seq_of(line));
      v.last_seq = std::to_string(seq_of(line));
      started = true;
      prev = sc->second;
    }
  }
  return v;
}

}  // namespace goober::logs
