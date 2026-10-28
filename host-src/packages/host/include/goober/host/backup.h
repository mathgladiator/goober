#pragma once

// Backups of a host's storage.
//
// Locally (PROGRESS.md Phase 3): every SQLite database through SQLite's online backup API, so
// each copy is consistent while the host keeps writing, and every other file copied as it is.
// Restore copies a backup into an empty storage directory.
//
// Off the machine (design/human/663.Backups.md; design/machine/019.Backups.md): sealed nightly
// archives, page-level database increments, and assets, in S3. Rehydrate builds a sandboxed
// devbox from any restore point.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "goober/aws/aws.h"
#include "goober/backup/backup.h"

namespace goober::host {

struct BackupReport {
  int databases = 0;
  int files = 0;
  int64_t bytes = 0;
};

struct BackupOptions {
  // Attachments' bytes (objects/$tenant/attachments/). The nightly archive leaves them out:
  // each one goes to the bucket once, on its own.
  bool assets = true;
};

// Whether a path in storage belongs in a backup: not logs, the dev mail outbox, the mail
// limits ledger, temporary files, the backup service's own state, or SQLite's sidecars.
bool backed_up(const std::string& rel, const BackupOptions& options);

// A consistent copy of one SQLite database while it is in use, durably written.
bool copy_database(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error);

// Copies `storage` into `dest`, which must not exist yet. Every file is fsynced, and
// BACKUP.json is written last, so a backup that has it is whole.
bool backup_storage(const std::filesystem::path& storage, const std::filesystem::path& dest, BackupReport* report,
                    std::string* error, const BackupOptions& options = {});

// Copies a backup into `storage`, which must be missing or empty. Run it with the host stopped.
bool restore_storage(const std::filesystem::path& backup, const std::filesystem::path& storage, BackupReport* report,
                     std::string* error);

// ---- Rehydrating from S3 ----

struct RehydrateOptions {
  aws::S3Location location;
  aws::Credentials credentials;
  backup::Key key{};
  std::filesystem::path dir;  // must be missing or empty; gets storage/, host.json, REHYDRATED.json
  int64_t at_ms = 0;          // the latest restore point at or before this; 0 for the latest
  bool keep_keys = false;     // keep the production key store (left out by default)
  uint16_t port = 8080;
};

struct RehydrateReport {
  std::string night, increment;  // the restore point
  int databases = 0, deltas = 0, files = 0, assets = 0, missing_assets = 0;
  std::vector<std::string> notes;
};

// The restore points in the bucket, oldest first.
std::optional<std::vector<backup::RestorePoint>> list_restore_points(aws::S3& s3, std::string* error);

// Builds a devbox from a restore point: the night's archive, the increment's databases and
// files, every stored attachment, each database checked; then a sandbox config (localhost,
// dev mailer, nothing leaves, no backups).
bool rehydrate(const RehydrateOptions& options, RehydrateReport* report, std::string* error);

}  // namespace goober::host
