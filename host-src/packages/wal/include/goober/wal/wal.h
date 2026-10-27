#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace goober::wal {

// One change to a file under the storage root.
struct FileChange {
  enum class Kind : uint8_t { kWrite = 1, kDelete = 2 };
  Kind kind = Kind::kWrite;
  std::string path;  // relative to the storage root
  std::string data;  // the whole file, for a write
};

// A bundle of file changes that land together.
using Bundle = std::vector<FileChange>;

struct RecoveryReport {
  size_t replayed_bundles = 0;
  size_t discarded_bundles = 0;  // bundles with no commit record
  bool torn_tail = false;        // a truncated or corrupt record ended the log
  // Committed bundles that still couldn't be applied, set aside under wal/failed/ so the
  // host starts anyway and an operator can look: "bundle 7: cannot apply change to ...".
  std::vector<std::string> failed;
};

// The host WAL for files (design/machine/005.FirstVersion.md §7).
//
// commit(bundle): append every change, append a commit record, fsync; apply each
// change durably; append a done record. A crash at any point leaves either no
// trace of the bundle or a committed bundle that recovery replays.
class HostWal {
 public:
  // Opens storage/wal/host.wal, replaying committed bundles that are not done.
  HostWal(std::filesystem::path storage_root, uint64_t truncate_after_bytes = 64ull << 20);
  ~HostWal();
  HostWal(const HostWal&) = delete;
  HostWal& operator=(const HostWal&) = delete;

  const RecoveryReport& recovery() const { return recovery_; }

  // Commits the bundle. Returns false, with `error` set, if it could not be made
  // durable; the bundle is then not applied. Paths must be safe relative paths.
  bool commit(const Bundle& bundle, std::string* error);

  const std::filesystem::path& root() const { return root_; }

  // Test hook: append and fsync the records and the commit, but don't apply the
  // bundle, as if the host died right after the commit record.
  bool commit_without_apply_for_test(const Bundle& bundle);

 private:
  bool append_records(uint64_t id, const Bundle& bundle, std::string* error);
  bool apply(const Bundle& bundle, std::string* error);
  void recover();
  void reopen(bool truncate);

  std::filesystem::path root_;
  std::filesystem::path file_;
  uint64_t truncate_after_;
  bool unapplied_ = false;  // a committed bundle failed to apply; the log must keep it
  std::mutex mu_;
  int fd_ = -1;
  uint64_t next_id_ = 1;
  RecoveryReport recovery_;
};

}  // namespace goober::wal
