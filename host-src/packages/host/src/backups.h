#pragma once

// Backups to S3 (design/human/663.Backups.md; design/machine/019.Backups.md), private to the
// host package. Three streams: attachments queued and uploaded once each; a nightly sealed
// tar.gz of the whole storage (attachments aside), kept keep-days; and, between nightlies,
// database increments: the pages that differ from the night's copy, plus any other file that
// changed, with a manifest per restore point. State lives in storage/backup/state.db.

#include <filesystem>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include "goober/aws/aws.h"
#include "goober/backup/backup.h"
#include "goober/config/config.h"
#include "goober/json/json.h"
#include "goober/sqlite/sqlite.h"

namespace goober::host {

class KeyStore;

class BackupService {
 public:
  using ConfigFn = std::function<std::shared_ptr<const config::HostConfig>()>;
  BackupService(ConfigFn config, KeyStore& keys, std::filesystem::path config_file);

  // Whatever is due at now_ms: assets, then a nightly or an increment. The backup thread
  // calls it every few seconds; tests call it with their own clock.
  void tick(int64_t now_ms);
  // An attachment was just stored: scan the ledgers at the next tick.
  void nudge();

  // Each stream on demand (operators, tests). false with `error` when it couldn't run.
  bool run_nightly(int64_t now_ms, std::string* error);
  bool run_increment(int64_t now_ms, std::string* error);
  // Uploads what's queued and due; returns how many went up.
  int run_assets(int64_t now_ms, std::string* error);
  // Rotated logs queued under logs/archive/ (design/machine/032.Logs.md § 2): each packed,
  // sealed, and put under logs/, its manifest beside it, then its queue link removed.
  int run_logs(int64_t now_ms, std::string* error);

  // Health is judged at now_ms (the wall clock, except in tests).
  boost::json::object status(bool with_points, int64_t now_ms);
  // The backup key, made on first use (keys/system/backup_key). nullopt with `error` if the
  // key store can't be written.
  std::optional<backup::Key> key(std::string* error);
  void confirm_key();

 private:
  std::unique_ptr<aws::S3> bucket(const config::HostConfig& cfg) const;
  sqlite::Db& state();
  std::string meta(const std::string& k);
  void set_meta(const std::string& k, const std::string& v);
  void record_error(const std::string& what, const std::string& error, int64_t now_ms);
  bool seal_and_put(aws::S3& s3, const backup::Key& key, const std::filesystem::path& file, const std::string& object,
                    std::string* error);
  void scan_ledgers(int64_t now_ms);
  void rotate(aws::S3& s3, const config::HostConfig& cfg, int64_t now_ms);

  ConfigFn config_;
  KeyStore& keys_;
  std::filesystem::path config_file_;
  std::recursive_mutex mu_;  // one stream at a time, and the state database
  std::unique_ptr<sqlite::Db> state_;
  int64_t scanned_ms_ = 0;
  std::atomic<bool> nudged_{false};  // set from request threads without the lock
};

}  // namespace goober::host
