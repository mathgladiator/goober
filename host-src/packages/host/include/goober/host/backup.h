#pragma once

// Online backups of a host's storage (PROGRESS.md Phase 3): every SQLite database through
// SQLite's backup API, so each copy is consistent while the host keeps writing, and every
// other file copied as it is. Restore copies a backup into an empty storage directory.

#include <filesystem>
#include <string>

namespace goober::host {

struct BackupReport {
  int databases = 0;
  int files = 0;
  int64_t bytes = 0;
};

// Copies `storage` into `dest`, which must not exist yet. Logs, mail waiting in the dev
// outbox (sign-in codes), and SQLite's -wal and -shm sidecars are left out.
bool backup_storage(const std::filesystem::path& storage, const std::filesystem::path& dest, BackupReport* report,
                    std::string* error);

// Copies a backup into `storage`, which must be missing or empty. Run it with the host stopped.
bool restore_storage(const std::filesystem::path& backup, const std::filesystem::path& storage, BackupReport* report,
                     std::string* error);

}  // namespace goober::host
