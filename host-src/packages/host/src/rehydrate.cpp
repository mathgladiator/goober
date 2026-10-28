// Rehydrating a devbox from S3 (design/human/663.Backups.md § Rehydrating on a devbox;
// design/machine/019.Backups.md §5).

#include <sqlite3.h>

#include <set>

#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/process.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/json/json.h"
#include "goober/sqlite/sqlite.h"

namespace goober::host {
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

bool download_unsealed(aws::S3& s3, const backup::Key& key, const std::string& object, const fs::path& work,
                       const fs::path& out, std::string* error) {
  fs::path sealed = work / "download.sealed";
  if (!s3.get_file(object, sealed, error)) return *error = object + ": " + *error, false;
  bool ok = backup::unseal_file(key, sealed, out, error);
  std::error_code ec;
  fs::remove(sealed, ec);
  return ok;
}

// The devbox's config: the production config with everything that reaches the world turned
// off, or a minimal one when the archive had none.
std::string devbox_config(const std::optional<std::string>& original, uint16_t port, std::vector<std::string>* notes) {
  bj::object cfg;
  if (original) {
    std::string err;
    if (auto v = json::parse(*original, &err); v && v->is_object()) {
      cfg = v->as_object();
    } else {
      notes->push_back("the archived host.json couldn't be read; wrote a minimal one");
    }
  }
  cfg["storage"] = "storage";
  cfg["listen"] = bj::object{{"address", "127.0.0.1"}, {"port", port}};
  cfg["sandbox"] = true;
  cfg.erase("backup");
  cfg.erase("public-url");
  if (auto* id = cfg.if_contains("identity"); id && id->is_object()) {
    id->as_object()["mailer"] = "dev";
    id->as_object()["cookie-secure"] = false;
    id->as_object().erase("smtp");
    id->as_object().erase("ses");
  }
  if (auto* source = cfg.if_contains("source"); source && source->is_object()) source->as_object().erase("vendor");
  return json::pretty(cfg) + "\n";
}

}  // namespace

std::optional<std::vector<backup::RestorePoint>> list_restore_points(aws::S3& s3, std::string* error) {
  std::vector<std::string> keys;
  for (const char* prefix : {"nightly/", "increments/"}) {
    auto objects = s3.list(prefix, error);
    if (!objects) return std::nullopt;
    for (const auto& o : *objects) keys.push_back(o.key);
  }
  return backup::restore_points(keys);
}

bool rehydrate(const RehydrateOptions& o, RehydrateReport* report, std::string* error) {
  std::error_code ec;
  if (fs::exists(o.dir) && !fs::is_empty(o.dir, ec)) return *error = o.dir.string() + " isn't empty", false;
  fs::create_directories(o.dir);
  fs::path work = o.dir / ".rehydrate";
  fs::create_directories(work);
  aws::S3 s3(o.location, [creds = o.credentials] { return std::optional(creds); }, work);
  auto points = list_restore_points(s3, error);
  if (!points) return false;
  auto point = backup::choose_point(*points, o.at_ms);
  if (!point) {
    return *error = points->empty() ? "the bucket holds no backups" : "no restore point at or before that moment", false;
  }
  report->night = point->night;
  report->increment = point->increment;

  // The night: its archive, unpacked.
  if (!download_unsealed(s3, o.key, backup::nightly_key(point->night), work, work / "archive.tar.gz", error)) return false;
  auto tar = base::run_process({"tar", "-xzf", (work / "archive.tar.gz").string(), "-C", o.dir.string(), "--no-same-owner"},
                               "", 64 << 10, 3600 * 1000);
  if (!tar.started || tar.exit_code != 0) return *error = "unpacking the night failed: " + tar.err.substr(0, 300), false;
  fs::remove(work / "archive.tar.gz", ec);
  fs::path storage = o.dir / "storage";
  if (!fs::is_directory(storage)) return *error = "the night's archive holds no storage", false;

  // The increment: each database rebuilt from the night's copy and its delta, the files that
  // changed since, and what was removed.
  if (!point->increment.empty()) {
    std::string prefix = backup::increment_prefix(point->night, point->increment);
    if (!download_unsealed(s3, o.key, prefix + "MANIFEST.json.sealed", work, work / "MANIFEST.json", error)) return false;
    std::string err;
    auto text = base::read_file(work / "MANIFEST.json");
    auto manifest = text ? json::parse(*text, &err) : std::nullopt;
    if (!manifest || !manifest->is_object()) return *error = "the increment's manifest can't be read", false;
    const auto& m = manifest->as_object();
    std::set<std::string> listed;
    for (const auto& kv : m.at("databases").as_object()) {
      std::string rel(kv.key());
      if (!base::is_safe_relative_path(rel)) return *error = "the manifest names an unsafe path: " + rel, false;
      listed.insert(rel);
      const auto& entry = kv.value().as_object();
      fs::path target = storage / rel;
      if (entry.at("delta").is_string()) {
        fs::path delta = work / "delta";
        if (!download_unsealed(s3, o.key, std::string(entry.at("delta").as_string()), work, delta, error)) return false;
        fs::create_directories(target.parent_path());
        bool had_base = fs::exists(target);
        if (!backup::apply_delta(had_base ? &target : nullptr, delta, target, error)) return *error = rel + ": " + *error, false;
        fs::remove(delta, ec);
        ++report->deltas;
      } else {
        auto sha = base::sha256_file_hex(target);
        if (!sha || *sha != std::string(entry.at("sha256").as_string())) {
          return *error = rel + " in the night's archive isn't what the increment expects", false;
        }
      }
    }
    // Databases the night had and the increment doesn't were removed in between.
    for (const auto& rel : base::list_files(storage)) {
      if (rel.ends_with(".db") && !listed.count(rel)) fs::remove(storage / rel, ec);
    }
    if (m.at("files").is_string()) {
      if (!download_unsealed(s3, o.key, std::string(m.at("files").as_string()), work, work / "files.tar.gz", error)) return false;
      auto files = base::run_process({"tar", "-xzf", (work / "files.tar.gz").string(), "-C", storage.string(), "--no-same-owner"},
                                     "", 64 << 10, 3600 * 1000);
      if (!files.started || files.exit_code != 0) return *error = "unpacking the changed files failed: " + files.err.substr(0, 300), false;
      ++report->files;
    }
    for (const auto& r : m.at("removed").as_array()) {
      std::string rel(r.as_string());
      if (base::is_safe_relative_path(rel)) fs::remove(storage / rel, ec);
    }
  }

  // Attachments: every one the restored ledgers say is stored.
  for (const auto& e : fs::directory_iterator(storage / "sqlite3", ec)) {
    std::string tenant = e.path().filename().string();
    if (!base::is_valid_name(tenant) || !fs::exists(e.path() / "internal.db")) continue;
    std::vector<std::pair<int64_t, std::string>> stored;
    try {
      sqlite::Db ledger(e.path() / "internal.db");
      sqlite::Stmt has(ledger, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'uploads'");
      if (!has.step()) continue;
      sqlite::Stmt s(ledger, "SELECT id, ext FROM uploads WHERE state = 'stored'");
      while (s.step()) stored.emplace_back(s.int64(0), s.text(1));
    } catch (const std::exception& ex) {
      report->notes.push_back(tenant + "'s attachment ledger can't be read: " + ex.what());
      continue;
    }
    for (const auto& [id, ext] : stored) {
      fs::path target = storage / "objects" / tenant / "attachments" / std::to_string(id);
      fs::create_directories(target.parent_path());
      std::string err;
      if (download_unsealed(s3, o.key, backup::asset_key(tenant, id, ext), work, target, &err)) {
        ++report->assets;
      } else {
        ++report->missing_assets;
        if (report->missing_assets <= 5) report->notes.push_back("attachment " + tenant + "/" + std::to_string(id) + ": " + err);
      }
    }
  }

  // Every database opens and passes SQLite's own check.
  for (const auto& rel : base::list_files(storage)) {
    if (!rel.ends_with(".db")) continue;
    ++report->databases;
    sqlite3* db = nullptr;
    std::string result;
    if (sqlite3_open_v2((storage / rel).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        result = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
      }
      sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (result != "ok") report->notes.push_back(rel + " fails SQLite's check: " + (result.empty() ? "can't open" : result));
  }

  // The production key store stays behind unless asked for: a devbox needs none of it.
  if (!o.keep_keys) fs::remove_all(storage / "keys", ec);
  auto original = base::read_file(o.dir / "host.json");
  if (!base::write_file_durable(o.dir / "host.json", devbox_config(original, o.port, &report->notes))) {
    return *error = "can't write the devbox's host.json", false;
  }
  base::write_file_durable(o.dir / "REHYDRATED.json",
                           json::pretty(bj::object{{"at", base::format_utc_ms(base::now_unix_ms())},
                                                   {"bucket", o.location.bucket},
                                                   {"prefix", o.location.prefix},
                                                   {"night", report->night},
                                                   {"increment", report->increment},
                                                   {"keys_kept", o.keep_keys},
                                                   {"sandbox", true}}));
  fs::remove_all(work, ec);
  return true;
}

}  // namespace goober::host
