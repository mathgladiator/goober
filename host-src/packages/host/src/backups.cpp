// Backups to S3 (design/human/663.Backups.md; design/machine/019.Backups.md).

#include "backups.h"

#include <iostream>
#include <set>

#include "goober/logs/logs.h"
#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/process.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "keys.h"

namespace goober::host {
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

constexpr int64_t kDayMs = 86400000;
constexpr int64_t kScanEveryMs = 30000;
constexpr int64_t kNightlyRetryMs = 15 * 60 * 1000;

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
-- The night's copy of each database: its page map, which increments are measured against.
CREATE TABLE IF NOT EXISTS night_dbs (rel TEXT PRIMARY KEY, page_size INTEGER NOT NULL, hashes TEXT NOT NULL,
                                      sha256 TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS night_files (rel TEXT PRIMARY KEY, sha256 TEXT NOT NULL);
-- What the last increment said about each database, and how to tell it hasn't changed since.
CREATE TABLE IF NOT EXISTS db_state (rel TEXT PRIMARY KEY, sig TEXT NOT NULL, sha256 TEXT NOT NULL, object TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS file_state (rel TEXT PRIMARY KEY, sig TEXT NOT NULL, sha256 TEXT NOT NULL);
-- Attachments: queued, uploaded, or gone (deleted here, kept in the bucket for keep-days).
CREATE TABLE IF NOT EXISTS assets (tenant TEXT NOT NULL, id INTEGER NOT NULL, ext TEXT NOT NULL, state TEXT NOT NULL,
                                   attempts INTEGER NOT NULL DEFAULT 0, next_ms INTEGER NOT NULL DEFAULT 0,
                                   queued_ms INTEGER NOT NULL, uploaded_ms INTEGER, gone_ms INTEGER, last_error TEXT,
                                   PRIMARY KEY (tenant, id));
)sql";

std::string hashes_hex(const backup::PageMap& map) {
  std::string out;
  out.reserve(map.hashes.size() * 32);
  for (const auto& h : map.hashes) out += base::hex_encode(std::string_view(reinterpret_cast<const char*>(h.data()), h.size()));
  return out;
}

backup::PageMap map_from_hex(uint32_t page_size, const std::string& hex) {
  backup::PageMap map;
  map.page_size = page_size;
  map.pages = hex.size() / 32;
  for (size_t i = 0; i + 32 <= hex.size(); i += 32) {
    std::array<uint8_t, 16> h{};
    for (size_t j = 0; j < 16; ++j) h[j] = uint8_t(std::stoi(hex.substr(i + 2 * j, 2), nullptr, 16));
    map.hashes.push_back(h);
  }
  return map;
}

// How a file looks from outside: size and change time, for it and its WAL. Unchanged means
// not worth reading.
std::string signature(const fs::path& p) {
  std::error_code ec;
  std::string sig;
  for (const fs::path& f : {p, fs::path(p.string() + "-wal")}) {
    auto size = fs::file_size(f, ec);
    if (ec) {
      sig += "-;";
      ec.clear();
      continue;
    }
    auto t = fs::last_write_time(f, ec).time_since_epoch().count();
    sig += std::to_string(size) + ":" + std::to_string(t) + ";";
  }
  return sig;
}

// Every file in storage a backup holds, relative and sorted, with assets left out.
std::vector<std::string> storage_files(const fs::path& storage) {
  std::vector<std::string> out;
  for (const auto& rel : base::list_files(storage)) {
    if (backed_up(rel, BackupOptions{.assets = false})) out.push_back(rel);
  }
  return out;
}

bool is_db(const std::string& rel) { return rel.ends_with(".db"); }

bool run_tar(const std::vector<std::string>& args, std::string* error) {
  std::vector<std::string> argv{"tar"};
  argv.insert(argv.end(), args.begin(), args.end());
  auto r = base::run_process(argv, "", 64 << 10, 3600 * 1000);
  if (!r.started) return *error = "could not run tar", false;
  if (r.timed_out || r.exit_code != 0) return *error = "tar failed: " + r.err.substr(0, 300), false;
  return true;
}

}  // namespace

BackupService::BackupService(ConfigFn config, KeyStore& keys, fs::path config_file)
    : config_(std::move(config)), keys_(keys), config_file_(std::move(config_file)) {}

sqlite::Db& BackupService::state() {
  if (!state_) {
    auto dir = config_()->storage / "backup";
    fs::create_directories(dir);
    state_ = std::make_unique<sqlite::Db>(dir / "state.db");
    state_->exec(kSchema);
  }
  return *state_;
}

std::string BackupService::meta(const std::string& k) {
  sqlite::Stmt s(state(), "SELECT v FROM meta WHERE k = ?");
  s.bind(1, k);
  return s.step() ? s.text(0) : "";
}

void BackupService::set_meta(const std::string& k, const std::string& v) {
  sqlite::Stmt s(state(), "INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT (k) DO UPDATE SET v = excluded.v");
  s.bind(1, k).bind(2, v).run();
}

void BackupService::record_error(const std::string& what, const std::string& error, int64_t now_ms) {
  std::lock_guard lock(mu_);
  std::cerr << "goober-host: backup " << what << ": " << error << "\n";
  set_meta("last_error", what + ": " + error);
  set_meta("last_error_ms", std::to_string(now_ms));
}

std::unique_ptr<aws::S3> BackupService::bucket(const config::HostConfig& cfg) const {
  aws::S3Location loc{cfg.backup.bucket, cfg.backup.region.empty() ? cfg.aws.region : cfg.backup.region,
                      cfg.backup.endpoint, cfg.backup.prefix};
  KeyStore* keys = &keys_;
  return std::make_unique<aws::S3>(loc, [keys] { return keys->aws_credentials(); }, cfg.storage / "tmp");
}

std::optional<backup::Key> BackupService::key(std::string* error) {
  if (auto existing = keys_.resolve("keys/system/backup_key", "")) {
    auto k = backup::parse_key(*existing);
    if (!k) *error = "keys/system/backup_key isn't 64 hex digits";
    return k;
  }
  // Made once; the operator must keep a copy elsewhere (goober-host backup key).
  auto k = backup::new_key();
  if (!keys_.set("system", "", "backup_key", backup::key_hex(k), base::now_unix_ms(), error)) return std::nullopt;
  return k;
}

void BackupService::confirm_key() {
  std::lock_guard lock(mu_);
  set_meta("key_confirmed_ms", std::to_string(base::now_unix_ms()));
}

void BackupService::nudge() { nudged_ = true; }

bool BackupService::seal_and_put(aws::S3& s3, const backup::Key& key, const fs::path& file, const std::string& object,
                                 std::string* error) {
  fs::path sealed = file.string() + ".sealed";
  bool ok = backup::seal_file(key, file, sealed, error, object) && s3.put_file(object, sealed, error);
  std::error_code ec;
  fs::remove(sealed, ec);
  return ok;
}

void BackupService::tick(int64_t now_ms) {
  auto cfg = config_();
  // A sandbox (a rehydrated devbox) never writes into anyone's bucket.
  if (!cfg->backup.enabled() || cfg->sandbox) return;
  std::string error;
  try {
    run_assets(now_ms, &error);
  } catch (const std::exception& e) {
    record_error("assets", e.what(), now_ms);
  }
  try {
    if (run_logs(now_ms, &error) < 0) record_error("logs", error, now_ms);
  } catch (const std::exception& e) {
    record_error("logs", e.what(), now_ms);
  }
  // Tonight's slot: today at nightly-at, or yesterday's if that hasn't come yet.
  int64_t slot = now_ms / kDayMs * kDayMs + int64_t(cfg->backup.nightly_minute) * 60000;
  if (slot > now_ms) slot -= kDayMs;
  // What's due is decided under the same lock the stream runs under, so an operator's
  // backup_run finishing just before can't make this tick run the same nightly again.
  std::lock_guard lock(mu_);
  int64_t last_nightly = std::atoll(meta("last_nightly_ms").c_str());
  int64_t last_attempt = std::atoll(meta("last_nightly_attempt_ms").c_str());
  int64_t last_increment = std::atoll(meta("last_increment_ms").c_str());
  try {
    if (last_nightly < slot) {
      // The first nightly runs at once (increments need a night); a failed one again in a while.
      if (now_ms - last_attempt >= kNightlyRetryMs) {
        if (!run_nightly(now_ms, &error)) record_error("nightly", error, now_ms);
      }
      return;
    }
    if (now_ms - std::max(last_increment, last_nightly) >= cfg->backup.databases_every_ms) {
      if (!run_increment(now_ms, &error)) record_error("increment", error, now_ms);
    }
  } catch (const std::exception& e) {
    record_error("tick", e.what(), now_ms);
  }
}

bool BackupService::run_nightly(int64_t now_ms, std::string* error) {
  std::lock_guard lock(mu_);
  auto cfg = config_();
  if (!cfg->backup.enabled()) return *error = "backups are off: set backup.s3.bucket in the config", false;
  if (cfg->sandbox) return *error = "this host is a sandbox: it never backs up", false;
  set_meta("last_nightly_attempt_ms", std::to_string(now_ms));
  auto key = this->key(error);
  if (!key) return false;
  auto s3 = bucket(*cfg);
  std::string at = backup::stamp(now_ms);
  fs::path work = cfg->storage / "tmp" / "backup-nightly";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } cleanup{work};

  // A consistent copy of storage (attachments go on their own), and the config beside it.
  BackupReport report;
  if (!backup_storage(cfg->storage, work / "storage", &report, error, BackupOptions{.assets = false})) return false;
  std::vector<std::string> members{"storage"};
  if (!config_file_.empty() && fs::exists(config_file_)) {
    fs::copy_file(config_file_, work / "host.json", fs::copy_options::overwrite_existing, ec);
    if (!ec) members.push_back("host.json");
  }
  // The night's page maps and file hashes: what increments compare against.
  std::vector<std::tuple<std::string, backup::PageMap>> dbs;
  std::vector<std::pair<std::string, std::string>> files;
  for (const auto& rel : base::list_files(work / "storage")) {
    if (rel == "BACKUP.json") continue;
    if (is_db(rel)) {
      auto map = backup::map_pages(work / "storage" / rel, error);
      if (!map) return false;
      dbs.emplace_back(rel, std::move(*map));
    } else {
      auto sha = base::sha256_file_hex(work / "storage" / rel);
      if (!sha) return *error = "can't read " + rel, false;
      files.emplace_back(rel, *sha);
    }
  }
  std::vector<std::string> tar{"-czf", (work / "archive.tar.gz").string(), "-C", work.string()};
  tar.insert(tar.end(), members.begin(), members.end());
  if (!run_tar(tar, error)) return false;
  if (!seal_and_put(*s3, *key, work / "archive.tar.gz", backup::nightly_key(at), error)) return false;

  auto& db = state();
  db.exec("BEGIN IMMEDIATE");
  try {
    db.exec("DELETE FROM night_dbs; DELETE FROM night_files; DELETE FROM db_state; DELETE FROM file_state;");
    for (const auto& [rel, map] : dbs) {
      sqlite::Stmt s(db, "INSERT INTO night_dbs (rel, page_size, hashes, sha256) VALUES (?, ?, ?, ?)");
      s.bind(1, rel).bind(2, int64_t(map.page_size)).bind(3, hashes_hex(map)).bind(4, map.sha256).run();
    }
    for (const auto& [rel, sha] : files) {
      sqlite::Stmt s(db, "INSERT INTO night_files (rel, sha256) VALUES (?, ?)");
      s.bind(1, rel).bind(2, sha).run();
    }
    set_meta("night", at);
    set_meta("last_nightly_ms", std::to_string(now_ms));
    set_meta("last_increment_ms", std::to_string(now_ms));
    set_meta("last_manifest", "");
    set_meta("last_files_fingerprint", "");
    set_meta("last_files_object", "");
    set_meta("last_nightly_bytes", std::to_string(fs::file_size(work / "archive.tar.gz", ec)));
    db.exec("COMMIT");
  } catch (...) {
    db.exec("ROLLBACK");
    throw;
  }
  set_meta("last_error", "");
  rotate(*s3, *cfg, now_ms);
  return true;
}

void BackupService::rotate(aws::S3& s3, const config::HostConfig& cfg, int64_t now_ms) {
  // Nights older than keep-days go, with their increments; the newest night always stays.
  int64_t cutoff = now_ms - int64_t(cfg.backup.keep_days) * kDayMs;
  std::string error;
  auto nightly = s3.list("nightly/", &error);
  auto increments = s3.list("increments/", &error);
  if (!nightly || !increments) {
    record_error("rotation", error, now_ms);
    return;
  }
  std::string newest;
  for (const auto& o : *nightly) newest = std::max(newest, o.key);
  auto old = [&](const std::string& stamp) {
    auto at = backup::parse_stamp(stamp);
    return at && *at < cutoff;
  };
  for (const auto& o : *nightly) {
    std::string s = o.key.substr(8, 20);
    if (o.key != newest && old(s) && !s3.remove(o.key, &error)) record_error("rotation", error, now_ms);
  }
  std::string newest_night = newest.size() >= 28 ? newest.substr(8, 20) : "";
  for (const auto& o : *increments) {
    std::string night = o.key.substr(11, 20);
    if (night != newest_night && old(night) && !s3.remove(o.key, &error)) record_error("rotation", error, now_ms);
  }
  // Archived logs are kept 400 days (032 § 2), by the day in their key: logs/$name/yyyy/mm/dd/...
  if (auto archived = s3.list("logs/", &error)) {
    int64_t log_cutoff = now_ms - 400 * kDayMs;
    for (const auto& o : *archived) {
      // The date is the three path segments before the file name.
      auto last = o.key.rfind('/');
      if (last == std::string::npos || last < 11) continue;
      std::string ymd = o.key.substr(last - 10, 10);  // yyyy/mm/dd
      auto at = backup::parse_moment(ymd.substr(0, 4) + "-" + ymd.substr(5, 2) + "-" + ymd.substr(8, 2));
      if (at && *at < log_cutoff && !s3.remove(o.key, &error)) record_error("rotation", error, now_ms);
    }
  } else {
    record_error("rotation", error, now_ms);
  }
  // Attachments deleted here more than keep-days ago leave the bucket too.
  std::vector<std::tuple<std::string, int64_t, std::string>> gone;
  {
    sqlite::Stmt s(state(), "SELECT tenant, id, ext FROM assets WHERE state = 'gone' AND gone_ms < ?");
    s.bind(1, cutoff);
    while (s.step()) gone.emplace_back(s.text(0), s.int64(1), s.text(2));
  }
  for (const auto& [tenant, id, ext] : gone) {
    if (s3.remove(backup::asset_key(tenant, id, ext), &error)) {
      sqlite::Stmt d(state(), "DELETE FROM assets WHERE tenant = ? AND id = ?");
      d.bind(1, tenant).bind(2, id).run();
    }
  }
}

int BackupService::run_logs(int64_t now_ms, std::string* error) {
  std::lock_guard lock(mu_);
  auto cfg = config_();
  if (!cfg->backup.enabled() || cfg->sandbox) return 0;
  fs::path queue = cfg->storage / "logs" / "archive";
  std::error_code ec;
  if (!fs::exists(queue, ec)) return 0;
  logs::finish_pending(queue);  // rotations since the last housekeeping
  std::vector<fs::path> files;
  for (auto it = fs::directory_iterator(queue, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    if (it->path().extension() == ".log") files.push_back(it->path());
  }
  if (files.empty()) return 0;
  auto key = this->key(error);
  if (!key) return -1;
  auto s3 = bucket(*cfg);
  std::sort(files.begin(), files.end());
  int put = 0;
  for (const auto& f : files) {
    fs::path manifest = f;
    manifest.replace_extension(".manifest.json");
    std::string log = "unknown", rotated = base::format_utc_ms(now_ms);
    if (auto text = base::read_file(manifest)) {
      std::string err;
      if (auto v = json::parse(*text, &err); v && v->is_object()) {
        if (auto* l = v->as_object().if_contains("log"); l && l->is_string()) log = std::string(l->as_string());
        if (auto* r = v->as_object().if_contains("rotated_at"); r && r->is_string()) rotated = std::string(r->as_string());
      }
    }
    for (auto& ch : log) {
      if (ch == '/') ch = '_';
    }
    std::string stem = f.stem().string();
    std::string prefix = "logs/" + log + "/" + rotated.substr(0, 4) + "/" + rotated.substr(5, 2) + "/" + rotated.substr(8, 2) + "/";
    fs::path packed = cfg->storage / "tmp" / (stem + ".log.tar.gz");
    fs::create_directories(packed.parent_path(), ec);
    if (!run_tar({"-czf", packed.string(), "-C", queue.string(), f.filename().string()}, error)) return -1;
    bool ok = seal_and_put(*s3, *key, packed, prefix + stem + ".log.tar.gz.sealed", error);
    fs::remove(packed, ec);
    if (!ok) return -1;
    // The manifest goes up as it is: it holds no log content, and lets the chain be checked
    // without the key.
    if (fs::exists(manifest, ec)) {
      if (!s3->put_file(prefix + stem + ".manifest.json", manifest, error)) return -1;
      fs::remove(manifest, ec);
    }
    fs::remove(f, ec);
    ++put;
    set_meta("last_logs_ms", std::to_string(now_ms));
  }
  return put;
}

bool BackupService::run_increment(int64_t now_ms, std::string* error) {
  std::lock_guard lock(mu_);
  auto cfg = config_();
  if (!cfg->backup.enabled()) return *error = "backups are off: set backup.s3.bucket in the config", false;
  if (cfg->sandbox) return *error = "this host is a sandbox: it never backs up", false;
  std::string night = meta("night");
  if (night.empty()) return *error = "no nightly yet: increments are measured against one", false;
  auto key = this->key(error);
  if (!key) return false;
  auto s3 = bucket(*cfg);
  std::string at = backup::stamp(now_ms);
  if (at <= night) return *error = "the increment would be no later than its night", false;
  std::string prefix = backup::increment_prefix(night, at);
  fs::path work = cfg->storage / "tmp" / "backup-increment";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } cleanup{work};
  auto& db = state();

  bj::object databases;
  std::vector<std::string> changed_files;
  bj::object changed_shas;
  std::set<std::string> seen_files;
  int uploads = 0, n = 0;
  for (const auto& rel : storage_files(cfg->storage)) {
    fs::path file = cfg->storage / rel;
    std::string sig = signature(file);
    if (!is_db(rel)) {
      seen_files.insert(rel);
      std::string sha;
      sqlite::Stmt prev(db, "SELECT sig, sha256 FROM file_state WHERE rel = ?");
      prev.bind(1, rel);
      if (prev.step() && prev.text(0) == sig) {
        sha = prev.text(1);
      } else {
        auto got = base::sha256_file_hex(file);
        if (!got) continue;  // gone meanwhile
        sha = *got;
        sqlite::Stmt up(db, "INSERT INTO file_state (rel, sig, sha256) VALUES (?, ?, ?) ON CONFLICT (rel) DO UPDATE SET "
                            "sig = excluded.sig, sha256 = excluded.sha256");
        up.bind(1, rel).bind(2, sig).bind(3, sha).run();
      }
      sqlite::Stmt base(db, "SELECT sha256 FROM night_files WHERE rel = ?");
      base.bind(1, rel);
      if (!base.step() || base.text(0) != sha) {
        changed_files.push_back(rel);
        changed_shas[rel] = sha;
      }
      continue;
    }
    // A database: unchanged since the last increment (by its look, then by its bytes), the
    // same as the night's, or a new delta against the night's copy.
    std::optional<std::tuple<std::string, std::string, std::string>> prev;
    {
      sqlite::Stmt s(db, "SELECT sig, sha256, object FROM db_state WHERE rel = ?");
      s.bind(1, rel);
      if (s.step()) prev.emplace(s.text(0), s.text(1), s.text(2));
    }
    std::optional<std::pair<backup::PageMap, std::string>> base_map;
    {
      sqlite::Stmt s(db, "SELECT page_size, hashes, sha256 FROM night_dbs WHERE rel = ?");
      s.bind(1, rel);
      if (s.step()) base_map.emplace(map_from_hex(uint32_t(s.int64(0)), s.text(1)), s.text(2));
    }
    auto entry = [&](const std::string& sha, const std::string& object) {
      databases[rel] = bj::object{{"sha256", sha}, {"delta", object.empty() ? bj::value(nullptr) : bj::value(object)}};
    };
    if (prev && std::get<0>(*prev) == sig) {
      entry(std::get<1>(*prev), std::get<2>(*prev));
      continue;
    }
    fs::path snap = work / "snapshot.db";
    if (!copy_database(file, snap, error)) return false;
    auto sha = base::sha256_file_hex(snap);
    if (!sha) return *error = "can't read the snapshot of " + rel, false;
    std::string object;
    if (prev && std::get<1>(*prev) == *sha) {
      object = std::get<2>(*prev);
    } else if (!(base_map && base_map->second == *sha)) {
      backup::DeltaInfo info;
      fs::path delta = work / "delta";
      if (!backup::write_delta(base_map ? &base_map->first : nullptr, snap, delta, &info, error)) return false;
      object = prefix + std::to_string(n++) + ".pages.sealed";
      if (!seal_and_put(*s3, *key, delta, object, error)) return false;
      ++uploads;
    }
    sqlite::Stmt up(db, "INSERT INTO db_state (rel, sig, sha256, object) VALUES (?, ?, ?, ?) ON CONFLICT (rel) DO UPDATE "
                        "SET sig = excluded.sig, sha256 = excluded.sha256, object = excluded.object");
    up.bind(1, rel).bind(2, sig).bind(3, *sha).bind(4, object).run();
    entry(*sha, object);
  }
  bj::array removed;
  {
    sqlite::Stmt s(db, "SELECT rel FROM night_files");
    while (s.step()) {
      if (!seen_files.count(s.text(0))) removed.push_back(bj::value(s.text(0)));
    }
  }
  // The other files that changed since the night, as one sealed tar.gz.
  bj::value files_object = nullptr;
  // The same changes as the last increment's: its archive of them still holds.
  std::string files_fingerprint = base::sha256_hex(json::dump(changed_shas));
  if (!changed_files.empty() && files_fingerprint == meta("last_files_fingerprint") && !meta("last_files_object").empty()) {
    files_object = bj::value(meta("last_files_object"));
  } else if (!changed_files.empty()) {
    std::string list;
    for (const auto& f : changed_files) list += f + "\n";
    if (!base::write_file_durable(work / "files.list", list)) return *error = "can't write the file list", false;
    if (!run_tar({"-czf", (work / "files.tar.gz").string(), "-C", cfg->storage.string(), "--ignore-failed-read",
                  "-T", (work / "files.list").string()},
                 error)) {
      return false;
    }
    std::string object = prefix + "files.tar.gz.sealed";
    if (!seal_and_put(*s3, *key, work / "files.tar.gz", object, error)) return false;
    files_object = bj::value(object);
    set_meta("last_files_fingerprint", files_fingerprint);
    set_meta("last_files_object", object);
    ++uploads;
  }
  bj::object manifest{{"format", 1}, {"night", night}, {"databases", databases}, {"files", files_object},
                      {"removed", removed}};
  // Nothing new since the last increment: no new restore point.
  std::string fingerprint =
      base::sha256_hex(json::dump(bj::object{{"d", databases}, {"r", removed}, {"f", changed_shas}}));
  if (uploads == 0 && fingerprint == meta("last_manifest")) {
    set_meta("last_increment_ms", std::to_string(now_ms));
    return true;
  }
  manifest["at"] = at;
  if (!base::write_file_durable(work / "MANIFEST.json", json::pretty(manifest))) return *error = "can't write the manifest", false;
  // The manifest goes last: a restore point exists only once everything it names is there.
  if (!seal_and_put(*s3, *key, work / "MANIFEST.json", prefix + "MANIFEST.json.sealed", error)) return false;
  set_meta("last_manifest", fingerprint);
  set_meta("last_increment_ms", std::to_string(now_ms));
  set_meta("last_increment", at);
  set_meta("last_error", "");
  return true;
}

void BackupService::scan_ledgers(int64_t now_ms) {
  // Every stored attachment in every tenant's ledger is queued once; one no longer stored is gone.
  auto cfg = config_();
  std::error_code ec;
  auto& db = state();
  for (const auto& e : fs::directory_iterator(cfg->storage / "sqlite3", ec)) {
    std::string tenant = e.path().filename().string();
    if (!base::is_valid_name(tenant) || !fs::exists(e.path() / "internal.db")) continue;
    try {
      sqlite::Db ledger(e.path() / "internal.db");
      sqlite::Stmt has(ledger, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'uploads'");
      if (!has.step()) continue;
      std::set<int64_t> stored;
      sqlite::Stmt s(ledger, "SELECT id, ext FROM uploads WHERE state = 'stored'");
      while (s.step()) {
        stored.insert(s.int64(0));
        sqlite::Stmt q(db, "INSERT OR IGNORE INTO assets (tenant, id, ext, state, queued_ms) VALUES (?, ?, ?, 'queued', ?)");
        q.bind(1, tenant).bind(2, s.int64(0)).bind(3, s.text(1)).bind(4, now_ms).run();
      }
      std::vector<int64_t> gone;
      sqlite::Stmt known(db, "SELECT id FROM assets WHERE tenant = ? AND state != 'gone'");
      known.bind(1, tenant);
      while (known.step()) {
        if (!stored.count(known.int64(0))) gone.push_back(known.int64(0));
      }
      for (int64_t id : gone) {
        sqlite::Stmt g(db, "UPDATE assets SET state = CASE state WHEN 'uploaded' THEN 'gone' ELSE 'dropped' END, gone_ms = ? "
                           "WHERE tenant = ? AND id = ?");
        g.bind(1, now_ms).bind(2, tenant).bind(3, id).run();
      }
      db.exec("DELETE FROM assets WHERE state = 'dropped'");  // never uploaded: nothing to keep
    } catch (const std::exception& ex) {
      record_error("assets", tenant + ": " + ex.what(), now_ms);
    }
  }
}

int BackupService::run_assets(int64_t now_ms, std::string* error) {
  std::lock_guard lock(mu_);
  auto cfg = config_();
  if (!cfg->backup.enabled() || cfg->sandbox) return 0;
  if (nudged_ || now_ms - scanned_ms_ >= kScanEveryMs) {
    nudged_ = false;
    scanned_ms_ = now_ms;
    scan_ledgers(now_ms);
  }
  std::vector<std::tuple<std::string, int64_t, std::string, int64_t>> due;
  {
    sqlite::Stmt s(state(), "SELECT tenant, id, ext, attempts FROM assets WHERE state = 'queued' AND next_ms <= ? "
                            "ORDER BY queued_ms LIMIT 25");
    s.bind(1, now_ms);
    while (s.step()) due.emplace_back(s.text(0), s.int64(1), s.text(2), s.int64(3));
  }
  if (due.empty()) return 0;
  auto key = this->key(error);
  if (!key) return 0;
  auto s3 = bucket(*cfg);
  fs::path work = cfg->storage / "tmp" / "backup-assets";
  fs::create_directories(work);
  int uploaded = 0;
  for (const auto& [tenant, id, ext, attempts] : due) {
    fs::path file = cfg->storage / "objects" / tenant / "attachments" / std::to_string(id);
    std::string err;
    bool ok = false;
    if (!fs::exists(file)) {
      err = "the attachment's bytes are missing";
    } else {
      fs::path copy = work / ("asset-" + std::to_string(id));
      std::error_code ec;
      fs::copy_file(file, copy, fs::copy_options::overwrite_existing, ec);
      ok = !ec && seal_and_put(*s3, *key, copy, backup::asset_key(tenant, id, ext), &err);
      if (ec) err = ec.message();
      fs::remove(copy, ec);
    }
    if (ok) {
      sqlite::Stmt u(state(), "UPDATE assets SET state = 'uploaded', uploaded_ms = ?, last_error = NULL WHERE tenant = ? AND id = ?");
      u.bind(1, now_ms).bind(2, tenant).bind(3, id).run();
      ++uploaded;
    } else {
      // Backing off: 10 s, doubling, at most an hour.
      int64_t backoff = std::min<int64_t>(10000LL << std::min<int64_t>(attempts, 20), 3600000);
      sqlite::Stmt u(state(), "UPDATE assets SET attempts = attempts + 1, next_ms = ?, last_error = ? WHERE tenant = ? AND id = ?");
      u.bind(1, now_ms + backoff).bind(2, err).bind(3, tenant).bind(4, id).run();
      *error = err;
    }
  }
  return uploaded;
}

bj::object BackupService::status(bool with_points, int64_t now_ms) {
  auto cfg = config_();
  bj::object out{{"enabled", cfg->backup.enabled() && !cfg->sandbox}};
  if (!cfg->backup.enabled()) {
    out["note"] = "Backups are off: set backup.s3 {bucket, region} in the config and the AWS keys (goober-host key set).";
    return out;
  }
  // Its own connection, and no lock: status answers while a nightly is uploading.
  fs::create_directories(cfg->storage / "backup");
  sqlite::Db reader(cfg->storage / "backup" / "state.db");
  reader.exec(kSchema);
  auto meta = [&](const char* k) {
    sqlite::Stmt s(reader, "SELECT v FROM meta WHERE k = ?");
    s.bind(1, k);
    return s.step() ? s.text(0) : std::string();
  };
  auto when = [&](const char* k) -> bj::value {
    auto v = meta(k);
    return v.empty() ? bj::value(nullptr) : bj::value(base::format_utc_ms(std::atoll(v.c_str())));
  };
  out["bucket"] = cfg->backup.bucket;
  out["prefix"] = cfg->backup.prefix;
  out["keep_days"] = cfg->backup.keep_days;
  out["night"] = meta("night");
  out["last_nightly_at"] = when("last_nightly_ms");
  out["last_increment_at"] = when("last_increment_ms");
  out["last_logs_at"] = when("last_logs_ms");
  out["last_error"] = meta("last_error");
  out["last_error_at"] = when("last_error_ms");
  out["key_confirmed_at"] = when("key_confirmed_ms");
  if (meta("key_confirmed_ms").empty()) {
    out["key_warning"] = "Nobody has confirmed keeping a copy of the backup key. Without it the backups can't be read: run "
                         "`goober-host backup key` on the server, store the key somewhere safe, then `goober-host backup key "
                         "--confirm`.";
  }
  bj::object assets;
  sqlite::Stmt s(reader, "SELECT state, count(*), min(queued_ms) FROM assets GROUP BY state");
  while (s.step()) {
    assets[s.text(0)] = s.int64(1);
    if (s.text(0) == "queued") assets["oldest_queued_at"] = base::format_utc_ms(s.int64(2));
  }
  out["assets"] = assets;
  int64_t last = std::atoll(meta("last_nightly_ms").c_str());
  out["healthy"] = last > 0 && now_ms - last < kDayMs + 2 * 3600000 && meta("last_error").empty();
  if (with_points) {
    std::string error;
    auto s3 = bucket(*cfg);
    if (auto points = list_restore_points(*s3, &error)) {
      bj::array list;
      for (const auto& p : *points) {
        list.push_back(bj::object{{"at", base::format_utc_ms(p.at_ms)}, {"night", p.night}, {"increment", p.increment}});
      }
      out["restore_points"] = list;
    } else {
      out["restore_points_error"] = error;
    }
  }
  return out;
}

}  // namespace goober::host
