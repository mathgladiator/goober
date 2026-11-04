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
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
// limits ledger, temporary files, the backup service's own state, source build caches, or
// SQLite's sidecars.
bool backed_up(const std::string& rel, const BackupOptions& options);

// The regular files under `root`, relative and sorted: with `filter`, only those a backup
// holds, and subtrees a backup never holds (tmp/, logs/, build caches) aren't entered, so what
// is in them can't stop a backup. A directory or file that must be read and can't be fails
// with its path, owner, and mode.
bool backup_files(const std::filesystem::path& root, const BackupOptions* filter, std::vector<std::string>* out,
                  std::string* error);
// A path with its owner and mode, for errors: "/srv/x (owner root, mode 0700)".
std::string describe_path(const std::filesystem::path& p);

// A consistent copy of one SQLite database while it is in use, durably written.
bool copy_database(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error);

// Copies `storage` into `dest`, which must not exist yet. Every file is fsynced, and
// BACKUP.json is written last, so a backup that has it is whole.
bool backup_storage(const std::filesystem::path& storage, const std::filesystem::path& dest, BackupReport* report,
                    std::string* error, const BackupOptions& options = {});

// Copies a backup into `storage`, which must be missing or empty. Run it with the host stopped.
bool restore_storage(const std::filesystem::path& backup, const std::filesystem::path& storage, BackupReport* report,
                     std::string* error);

// ---- Local nightly copies (design/machine/046.Operations.md § 9 A12, 045.Limits.md) ----

// A directory name like 2026-11-03: how setup's goober-backup timer names each night's copy.
bool is_dated_copy_name(std::string_view name);
// How many nights stay on the machine: 3 when S3 backups are on (the bucket holds the rest),
// 14 when they're off.
int local_copies_to_keep(bool s3_backups);
// The directory beside storage that setup's timer backs up into: <storage's parent>/backups.
std::filesystem::path local_copies_dir(const std::filesystem::path& storage);
// `dest` without a trailing separator when it is named YYYY-MM-DD, else empty.
std::filesystem::path dated_copy_dir(const std::filesystem::path& dest);
// After a backup into `dest`: when `dest` is named YYYY-MM-DD (a trailing slash or not), the
// dated copies beside it (directories named YYYY-MM-DD holding a BACKUP.json, so ones a backup
// made) are removed but for the newest by name, `keep` in all with `dest`, which always stays.
// Nothing else beside it (files, links, other names, dated directories without BACKUP.json) is
// touched. Answers the names removed, oldest first.
std::vector<std::string> prune_local_copies(const std::filesystem::path& dest, int keep, std::string* error);
// The dated copies in `dir`: how many, and their bytes (BACKUP.json's count for a whole copy,
// measured for one a failed run left).
struct LocalCopies {
  int copies = 0;
  int64_t bytes = 0;
};
LocalCopies local_copies(const std::filesystem::path& dir);

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
  int64_t ids_from = 0, ids_to = 0;  // the id counter, moved past what was restored
  std::vector<std::string> notes;
};

// The restore points in the bucket, oldest first.
std::optional<std::vector<backup::RestorePoint>> list_restore_points(aws::S3& s3, std::string* error);

// Builds a devbox from a restore point: the night's archive, the increment's databases and
// files, every stored attachment, each database checked; then a sandbox config (localhost,
// dev mailer, nothing leaves, no backups).
bool rehydrate(const RehydrateOptions& options, RehydrateReport* report, std::string* error);

// After a restore (design/machine/038.Ids.md § 6): moves the id counter in `storage` past the
// highest id any restored database holds, then ids::kMargin further, so no id that left the
// host after the last backup is minted again. Run with the host stopped. Returns the counter
// before and after.
std::pair<int64_t, int64_t> move_ids_past_restore(const std::filesystem::path& storage);

// The source mirror (design/machine/020.Source.md § 10), read back: every sealed patch and
// snapshot under source/ (or one project's), unsealed into DIR/PROJECT/patches/000042.patch and
// DIR/PROJECT/snapshots/000042.tar.gz. Files already there are kept, so a second pull fetches
// only what's new.
struct SourceMirrorOptions {
  aws::S3Location location;
  aws::Credentials credentials;
  backup::Key key{};
  std::filesystem::path dir;
  std::string project;  // "" for every project
};
struct SourceMirrorReport {
  std::map<std::string, std::pair<int, int>> projects;  // project -> patches, snapshots
  int fetched = 0;
};
bool pull_source_mirror(const SourceMirrorOptions& options, SourceMirrorReport* report, std::string* error);

}  // namespace goober::host
