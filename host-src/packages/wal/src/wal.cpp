#include "goober/wal/wal.h"
#include "goober/json/json.h"
#include "goober/base/encoding.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <stdexcept>

#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"

namespace goober::wal {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {
enum RecordKind : uint8_t { kWrite = 1, kDelete = 2, kCommit = 3, kDone = 4 };

void put_u32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(char(v >> (8 * i)));
}
void put_u64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(char(v >> (8 * i)));
}
bool get_u32(std::string_view& in, uint32_t& v) {
  if (in.size() < 4) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(in[i])) << (8 * i);
  in.remove_prefix(4);
  return true;
}
bool get_u64(std::string_view& in, uint64_t& v) {
  if (in.size() < 8) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(in[i])) << (8 * i);
  in.remove_prefix(8);
  return true;
}

std::string encode_record(uint8_t kind, uint64_t bundle, std::string_view path, std::string_view data) {
  std::string payload;
  payload.push_back(char(kind));
  put_u64(payload, bundle);
  put_u32(payload, uint32_t(path.size()));
  payload += path;
  put_u32(payload, uint32_t(data.size()));
  payload += data;
  std::string framed;
  put_u32(framed, uint32_t(payload.size()));
  put_u32(framed, base::crc32(payload));
  framed += payload;
  return framed;
}

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
}  // namespace

HostWal::HostWal(fs::path storage_root, uint64_t truncate_after_bytes)
    : root_(std::move(storage_root)), file_(root_ / "wal" / "host.wal"), truncate_after_(truncate_after_bytes) {
  fs::create_directories(file_.parent_path());
  recover();
}

HostWal::~HostWal() {
  if (fd_ >= 0) ::close(fd_);
}

void HostWal::reopen(bool truncate) {
  if (fd_ >= 0) ::close(fd_);
  int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | (truncate ? O_TRUNC : 0);
  fd_ = ::open(file_.c_str(), flags, 0644);
  if (fd_ < 0) throw std::runtime_error("cannot open " + file_.string());
  if (truncate) ::fsync(fd_);
}

void HostWal::recover() {
  auto text = base::read_file(file_).value_or("");
  std::string_view in(text);
  std::map<uint64_t, Bundle> pending;
  std::map<uint64_t, bool> committed;
  std::vector<uint64_t> order;
  uint64_t max_id = 0;
  while (!in.empty()) {
    uint32_t len = 0, crc = 0;
    std::string_view probe = in;
    if (!get_u32(probe, len) || !get_u32(probe, crc) || probe.size() < len) {
      recovery_.torn_tail = true;
      break;
    }
    std::string_view payload = probe.substr(0, len);
    if (base::crc32(payload) != crc) {
      recovery_.torn_tail = true;
      break;
    }
    in = probe.substr(len);
    if (payload.empty()) {
      recovery_.torn_tail = true;
      break;
    }
    uint8_t kind = uint8_t(payload[0]);
    payload.remove_prefix(1);
    uint64_t id = 0;
    uint32_t path_len = 0, data_len = 0;
    if (!get_u64(payload, id) || !get_u32(payload, path_len) || payload.size() < path_len) {
      recovery_.torn_tail = true;
      break;
    }
    std::string path(payload.substr(0, path_len));
    payload.remove_prefix(path_len);
    if (!get_u32(payload, data_len) || payload.size() < data_len) {
      recovery_.torn_tail = true;
      break;
    }
    std::string data(payload.substr(0, data_len));
    if (id > max_id) max_id = id;
    switch (kind) {
      case kWrite:
      case kDelete:
        if (!pending.count(id)) order.push_back(id);
        pending[id].push_back(FileChange{kind == kWrite ? FileChange::Kind::kWrite : FileChange::Kind::kDelete,
                                         std::move(path), std::move(data)});
        break;
      case kCommit:
        committed[id] = true;
        if (!pending.count(id)) {
          pending[id];
          order.push_back(id);
        }
        break;
      case kDone:
        pending.erase(id);
        committed.erase(id);
        break;
      default:
        recovery_.torn_tail = true;
        in = {};
        break;
    }
  }
  for (uint64_t id : order) {
    auto it = pending.find(id);
    if (it == pending.end()) continue;
    if (committed.count(id)) {
      std::string err;
      if (!apply(it->second, &err)) {
        // Set aside rather than refuse to start: one bad bundle shouldn't brick the host.
        bj::array changes;
        for (const auto& c : it->second) {
          changes.push_back(bj::object{{"kind", c.kind == FileChange::Kind::kWrite ? "write" : "delete"},
                                       {"path", c.path},
                                       {"data_base64", base::base64_encode(c.data)}});
        }
        base::write_file_durable(root_ / "wal" / "failed" / (std::to_string(id) + ".json"),
                                 json::pretty(bj::object{{"bundle", id}, {"error", err}, {"changes", changes}}));
        recovery_.failed.push_back("bundle " + std::to_string(id) + ": " + err);
        continue;
      }
      ++recovery_.replayed_bundles;
    } else {
      ++recovery_.discarded_bundles;
    }
  }
  next_id_ = max_id + 1;
  // Everything committed is now applied, so the log can start empty.
  reopen(true);
}

bool HostWal::append_records(uint64_t id, const Bundle& bundle, std::string* error) {
  std::string buf;
  for (const auto& change : bundle) {
    if (!base::is_safe_relative_path(change.path)) {
      *error = "unsafe path in bundle: " + change.path;
      return false;
    }
    buf += encode_record(change.kind == FileChange::Kind::kWrite ? kWrite : kDelete, id, change.path, change.data);
  }
  buf += encode_record(kCommit, id, "", "");
  struct stat before {};
  bool sized = ::fstat(fd_, &before) == 0;
  if (!write_all(fd_, buf) || ::fsync(fd_) != 0) {
    // A part-written bundle would end the log for recovery, which stops at the first torn
    // record, losing every bundle committed after it: cut it off.
    if (sized && ::ftruncate(fd_, before.st_size) == 0) ::fsync(fd_);
    *error = "cannot write the host WAL";
    return false;
  }
  return true;
}

bool HostWal::apply(const Bundle& bundle, std::string* error) {
  for (const auto& change : bundle) {
    if (!base::is_safe_relative_path(change.path)) {
      *error = "unsafe path: " + change.path;
      return false;
    }
    fs::path target = root_ / change.path;
    bool ok = change.kind == FileChange::Kind::kWrite ? base::write_file_durable(target, change.data)
                                                      : base::remove_file_durable(target);
    if (!ok) {
      *error = "cannot apply change to " + change.path;
      return false;
    }
  }
  return true;
}

bool HostWal::commit(const Bundle& bundle, std::string* error) {
  std::lock_guard lock(mu_);
  uint64_t id = next_id_++;
  if (!append_records(id, bundle, error)) return false;
  if (!apply(bundle, error)) {
    // The bundle is committed; recovery will retry it on the next start, so the log
    // keeps it until then.
    unapplied_ = true;
    return false;
  }
  std::string done = encode_record(kDone, id, "", "");
  write_all(fd_, done);
  struct stat st {};
  if (!unapplied_ && ::fstat(fd_, &st) == 0 && uint64_t(st.st_size) > truncate_after_) {
    // Every bundle is applied under this lock, and none failed, so nothing is in flight.
    ::fsync(fd_);
    reopen(true);
  }
  return true;
}

bool HostWal::commit_without_apply_for_test(const Bundle& bundle) {
  std::lock_guard lock(mu_);
  std::string err;
  return append_records(next_id_++, bundle, &err);
}

}  // namespace goober::wal
