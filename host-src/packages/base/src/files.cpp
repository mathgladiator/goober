#include "goober/base/files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>

#include "goober/base/random.h"

namespace goober::base {
namespace fs = std::filesystem;

namespace {
bool write_all(int fd, std::string_view data) {
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = ::write(fd, data.data() + done, data.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += size_t(n);
  }
  return true;
}

bool fsync_dir(const fs::path& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}
}  // namespace

bool is_safe_relative_path(std::string_view path) {
  // Within what every filesystem accepts: 255 bytes a name, 4096 a path.
  if (path.empty() || path.front() == '/' || path.size() > 4096) return false;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    std::string_view seg = path.substr(start, end - start);
    if (seg.empty() || seg == "." || seg == ".." || seg.size() > 255) return false;
    for (char c : seg) {
      if (c == '\0' || c == '\\') return false;
    }
    start = end + 1;
  }
  return true;
}

std::optional<std::string> read_file(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return ss.str();
}

std::optional<fs::path> write_private_file(const fs::path& dir, std::string_view prefix, std::string_view text) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  uint8_t rnd[12];
  os_random_fill(rnd);
  static const char* kHex = "0123456789abcdef";
  std::string name(prefix);
  for (uint8_t b : rnd) name += {kHex[b >> 4], kHex[b & 15]};
  fs::path file = dir / name;
  int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return std::nullopt;
  size_t done = 0;
  while (done < text.size()) {
    ssize_t w = ::write(fd, text.data() + done, text.size() - done);
    if (w <= 0) break;
    done += size_t(w);
  }
  ::close(fd);
  if (done != text.size()) {
    fs::remove(file, ec);
    return std::nullopt;
  }
  return file;
}

bool write_file_durable(const fs::path& path, std::string_view data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return false;
  fs::path tmp = path;
  tmp += ".tmp-" + std::to_string(os_random_u64());
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  bool ok = write_all(fd, data) && ::fsync(fd) == 0;
  ok = (::close(fd) == 0) && ok;
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return fsync_dir(path.parent_path());
}

bool remove_file_durable(const fs::path& path) {
  if (::unlink(path.c_str()) != 0) return errno == ENOENT;
  return fsync_dir(path.parent_path());
}

std::vector<std::string> list_files(const fs::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return out;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      std::string rel = fs::relative(it->path(), root, ec).generic_string();
      if (rel.find(".tmp-") == std::string::npos) out.push_back(rel);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool append_line(const fs::path& path, std::string_view line, uint64_t rotate_bytes) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  // Logs rotate past kLogRotateBytes, keeping three old files, so they can't fill the disk.
  struct stat st {};
  if (::fstat(fd, &st) == 0 && uint64_t(st.st_size) + line.size() + 1 > rotate_bytes) {
    ::close(fd);
    static std::mutex rotating;
    std::lock_guard lock(rotating);
    struct stat again {};
    if (::stat(path.c_str(), &again) == 0 && uint64_t(again.st_size) + line.size() + 1 > rotate_bytes) {
      for (int i = kLogsKept - 1; i >= 1; --i) {
        fs::rename(path.string() + "." + std::to_string(i), path.string() + "." + std::to_string(i + 1), ec);
      }
      fs::rename(path, path.string() + ".1", ec);
    }
    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return false;
  }
  std::string buf(line);
  buf.push_back('\n');
  bool ok = write_all(fd, buf);
  ::close(fd);
  return ok;
}

std::vector<std::string> tail_lines(const fs::path& path, size_t limit) {
  std::ifstream in(path);
  std::deque<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(std::move(line));
    if (lines.size() > limit) lines.pop_front();
  }
  return {lines.begin(), lines.end()};
}

}  // namespace goober::base
