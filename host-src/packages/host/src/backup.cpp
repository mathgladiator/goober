#include "goober/host/backup.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/json/json.h"

namespace goober::host {
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

// Flushes a file or directory to disk.
bool fsync_path(const fs::path& p) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

bool copy_tree(const fs::path& from, const fs::path& to, bool backup, const BackupOptions& options, BackupReport* report,
               std::string* error) {
  std::vector<std::string> rels;
  if (!backup_files(from, backup ? &options : nullptr, &rels, error)) return false;
  std::error_code ec;
  std::set<fs::path> dirs{to};
  for (const auto& r : rels) {
    if (!backup && r == "BACKUP.json") continue;
    fs::path source = from / r;
    fs::path target = to / r;
    fs::create_directories(target.parent_path());
    for (fs::path d = target.parent_path(); d != to && d.has_relative_path(); d = d.parent_path()) dirs.insert(d);
    if (backup && source.extension() == ".db") {
      if (!copy_database(source, target, error)) return false;
      ++report->databases;
    } else {
      if (!fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec)) {
        *error = "cannot copy " + describe_path(source) + ": " + ec.message();
        return false;
      }
      if (!fsync_path(target)) return *error = "cannot flush " + target.string(), false;
      ++report->files;
    }
    report->bytes += int64_t(fs::file_size(target, ec));
  }
  // The directories too, so every name is durable before BACKUP.json says the copy is whole.
  for (const auto& d : dirs) fsync_path(d);
  return true;
}

}  // namespace

std::string describe_path(const fs::path& p) {
  struct stat st {};
  if (::lstat(p.c_str(), &st) != 0) return p.string();
  char mode[8];
  std::snprintf(mode, sizeof mode, "%04o", unsigned(st.st_mode & 07777));
  std::string owner = std::to_string(st.st_uid);
  if (const passwd* pw = ::getpwuid(st.st_uid)) owner = pw->pw_name;
  return p.string() + " (owner " + owner + ", mode " + mode + ")";
}

bool backup_files(const fs::path& root, const BackupOptions* filter, std::vector<std::string>* out, std::string* error) {
  out->clear();
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return true;
  if (::access(root.c_str(), R_OK | X_OK) != 0) return *error = "cannot read " + describe_path(root), false;
  std::string base = root.generic_string();
  if (!base.empty() && base.back() != '/') base += '/';
  auto rel_of = [&](const fs::path& p) {
    std::string full = p.generic_string();
    return full.starts_with(base) ? full.substr(base.size()) : full;
  };
  // A subtree a backup never holds (tmp/, logs/, a build cache) is never entered: what's in it
  // can't stop a backup, whoever made it.
  auto skipped = [&](const std::string& rel_dir) { return filter && !backed_up(rel_dir + "/probe", *filter); };
  for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();) {
    if (ec) break;
    const auto& entry = *it;
    std::string rel = rel_of(entry.path());
    std::error_code e2;
    if (entry.is_directory(e2) && !entry.is_symlink(e2)) {
      if (skipped(rel)) {
        it.disable_recursion_pending();
      } else if (::access(entry.path().c_str(), R_OK | X_OK) != 0) {
        return *error = "cannot read " + describe_path(entry.path()) +
                        ": the host's user must own everything in storage (a deploy fixes this)",
               false;
      }
    } else if (entry.is_regular_file(e2) && !entry.is_symlink(e2)) {
      if (!filter || backed_up(rel, *filter)) {
        if (::access(entry.path().c_str(), R_OK) != 0) {
          return *error = "cannot read " + describe_path(entry.path()) +
                          ": the host's user must own everything in storage (a deploy fixes this)",
                 false;
        }
        out->push_back(std::move(rel));
      }
    }
    it.increment(ec);
  }
  if (ec) return *error = "cannot read " + root.string() + ": " + ec.message(), false;
  std::sort(out->begin(), out->end());
  return true;
}

bool backed_up(const std::string& r, const BackupOptions& options) {
  if (r.starts_with("logs/") || r.starts_with("mail/") || r.starts_with("tmp/") || r.starts_with("backup/")) return false;
  // Build caches are rebuilt from source; they'd make every nightly hundreds of MB larger.
  if (r.starts_with("source/cache/")) return false;
  if (!options.assets && r.starts_with("objects/") && r.find("/attachments/") != std::string::npos) return false;
  if (r.find(".tmp") != std::string::npos || r.ends_with(".unsealing") || r.ends_with(".applying")) return false;
  return !(r.ends_with(".db-wal") || r.ends_with(".db-shm") || r.ends_with(".db-journal"));
}

// SQLite's online backup: a consistent copy even while the host writes the source.
bool copy_database(const fs::path& from, const fs::path& to, std::string* error) {
  sqlite3* src = nullptr;
  sqlite3* dst = nullptr;
  auto close_both = [&] {
    if (src) sqlite3_close(src);
    if (dst) sqlite3_close(dst);
  };
  std::error_code ec;
  fs::remove(to, ec);
  if (sqlite3_open_v2(from.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
      sqlite3_open_v2(to.c_str(), &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    *error = "cannot open " + from.string() + " for backup";
    close_both();
    return false;
  }
  sqlite3_busy_timeout(src, 5000);
  sqlite3_backup* b = sqlite3_backup_init(dst, "main", src, "main");
  if (!b) {
    *error = "cannot back up " + from.string() + ": " + sqlite3_errmsg(dst);
    close_both();
    return false;
  }
  int rc;
  do {
    rc = sqlite3_backup_step(b, 256);
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) sqlite3_sleep(50);
  } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
  sqlite3_backup_finish(b);
  bool ok = rc == SQLITE_DONE && sqlite3_errcode(dst) == SQLITE_OK;
  if (!ok) *error = "backing up " + from.string() + " failed: " + sqlite3_errmsg(dst);
  close_both();
  // The copy's journal mode is the source's; the file itself is complete once closed.
  fs::remove(to.string() + "-wal", ec);
  fs::remove(to.string() + "-shm", ec);
  if (ok && !fsync_path(to)) return *error = "cannot flush " + to.string(), false;
  return ok;
}

bool backup_storage(const fs::path& storage, const fs::path& dest, BackupReport* report, std::string* error,
                    const BackupOptions& options) {
  if (!fs::is_directory(storage)) return *error = "no storage at " + storage.string(), false;
  if (fs::exists(dest)) return *error = dest.string() + " already exists; back up into a new directory", false;
  fs::create_directories(dest);
  if (!copy_tree(storage, dest, true, options, report, error)) return false;
  base::write_file_durable(dest / "BACKUP.json",
                           json::pretty(bj::object{{"at", base::format_utc_ms(base::now_unix_ms())},
                                                   {"from", fs::absolute(storage).string()},
                                                   {"databases", report->databases},
                                                   {"files", report->files},
                                                   {"bytes", report->bytes}}));
  return true;
}

bool restore_storage(const fs::path& backup, const fs::path& storage, BackupReport* report, std::string* error) {
  if (!fs::exists(backup / "BACKUP.json")) return *error = backup.string() + " isn't a goober backup", false;
  std::error_code ec;
  if (fs::exists(storage) && !fs::is_empty(storage, ec)) {
    return *error = storage.string() + " isn't empty; move it aside first (and stop the host)", false;
  }
  fs::create_directories(storage);
  return copy_tree(backup, storage, false, BackupOptions{}, report, error);
}

}  // namespace goober::host
