#include "goober/ids/ids.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <stdexcept>

#include "goober/base/time.h"
#include "goober/sqlite/sqlite.h"

namespace goober::ids {
namespace fs = std::filesystem;

namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS id_counter (only INTEGER PRIMARY KEY CHECK (only = 1), next_id INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS id_ranges (
  id_start INTEGER PRIMARY KEY, length INTEGER NOT NULL, vended_to TEXT NOT NULL, vended_at INTEGER NOT NULL);
)sql";

int64_t read_counter(sqlite::Db& db) {
  sqlite::Stmt s(db, "SELECT next_id FROM id_counter WHERE only = 1");
  return s.step() ? s.int64(0) : 0;
}

}  // namespace

Service::Service(const fs::path& file, std::string host, int64_t floor) : host_(std::move(host)) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  db_ = std::make_unique<sqlite::Db>(file);
  db_->exec("PRAGMA journal_mode = WAL");
  db_->exec("PRAGMA synchronous = FULL");
  db_->exec(kSchema);
  db_->exec("BEGIN IMMEDIATE");
  try {
    int64_t counter = read_counter(*db_);
    if (counter == 0) {
      sqlite::Stmt s(*db_, "INSERT INTO id_counter (only, next_id) VALUES (1, ?)");
      s.bind(1, std::max(kFirstId, floor)).run();
      created_ = true;
    } else if (counter < kFirstId) {
      // A counter made when the first id was 2^32, before anything was minted from it: it only
      // moves up, to the first id as it is now.
      sqlite::Stmt s(*db_, "UPDATE id_counter SET next_id = ? WHERE only = 1");
      s.bind(1, kFirstId).run();
    }
    db_->exec("COMMIT");
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
}

Service::~Service() = default;

void Service::take_range() {
  // The counter moves, and the range is recorded, in one durable transaction before any id
  // of it is handed out: a crash right after loses the range, never vends it twice.
  db_->exec("BEGIN IMMEDIATE");
  try {
    int64_t start = read_counter(*db_);
    if (start <= 0) throw std::runtime_error("the id counter is missing");
    if (start > INT64_MAX - kRangeLength) throw std::runtime_error("the id space is spent");
    sqlite::Stmt up(*db_, "UPDATE id_counter SET next_id = ? WHERE only = 1");
    up.bind(1, start + kRangeLength).run();
    sqlite::Stmt rec(*db_, "INSERT INTO id_ranges (id_start, length, vended_to, vended_at) VALUES (?, ?, ?, ?)");
    rec.bind(1, start).bind(2, kRangeLength).bind(3, host_).bind(4, base::now_unix_ms()).run();
    db_->exec("COMMIT");
    next_ = start;
    end_ = start + kRangeLength;
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
}

int64_t Service::next() {
  std::lock_guard lock(mu_);
  if (next_ >= end_) take_range();
  return next_++;
}

void Service::move_past(int64_t floor) {
  std::lock_guard lock(mu_);
  db_->exec("BEGIN IMMEDIATE");
  try {
    if (read_counter(*db_) < floor) {
      sqlite::Stmt up(*db_, "UPDATE id_counter SET next_id = ? WHERE only = 1");
      up.bind(1, floor).run();
    }
    db_->exec("COMMIT");
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
  if (next_ < floor) next_ = end_ = 0;  // the rest of the range is below the floor: dropped
}

int64_t Service::counter() {
  std::lock_guard lock(mu_);
  return read_counter(*db_);
}

std::pair<int64_t, int64_t> Service::current() {
  std::lock_guard lock(mu_);
  return {next_, end_};
}

std::vector<Range> Service::ranges(int limit) {
  std::lock_guard lock(mu_);
  std::vector<Range> out;
  sqlite::Stmt s(*db_, "SELECT id_start, length, vended_to, vended_at FROM id_ranges ORDER BY id_start DESC LIMIT ?");
  s.bind(1, int64_t(limit));
  while (s.step()) out.push_back({s.int64(0), s.int64(1), s.text(2), s.int64(3)});
  return out;
}

int64_t highest_id(const fs::path& file) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return 0;
  }
  int64_t best = 0;
  // Runs `sql`, with `arg` bound to its one parameter if it has one, calling `each` per row.
  auto query = [&](const std::string& sql, auto&& each, const std::string& arg = "") {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) {
      sqlite3_finalize(s);
      return;
    }
    if (sqlite3_bind_parameter_count(s) == 1) sqlite3_bind_text(s, 1, arg.data(), int(arg.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) each(s);
    sqlite3_finalize(s);
  };
  auto quote = [](const std::string& name) {
    std::string q = "\"";
    for (char c : name) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
  };
  std::vector<std::string> tables;
  query("SELECT name FROM sqlite_master WHERE type = 'table'", [&](sqlite3_stmt* s) {
    tables.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)));
  });
  for (const auto& t : tables) {
    if (t == "sqlite_sequence") {
      query("SELECT max(seq) FROM sqlite_sequence", [&](sqlite3_stmt* s) { best = std::max<int64_t>(best, sqlite3_column_int64(s, 0)); });
      continue;
    }
    if (t.starts_with("sqlite_")) continue;
    // The one integer column that is the whole primary key, if there is one.
    std::string key;
    int keys = 0;
    query("SELECT name, type, pk FROM pragma_table_info(?)",
          [&](sqlite3_stmt* s) {
            if (sqlite3_column_int(s, 2) == 0) return;
            ++keys;
            std::string type = reinterpret_cast<const char*>(sqlite3_column_text(s, 1));
            for (auto& c : type) c = char(std::toupper(static_cast<unsigned char>(c)));
            if (type == "INT" || type == "INTEGER") key = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
          },
          t);
    if (keys != 1 || key.empty()) continue;
    query("SELECT max(" + quote(key) + ") FROM " + quote(t), [&](sqlite3_stmt* s) {
      if (sqlite3_column_type(s, 0) == SQLITE_INTEGER) best = std::max<int64_t>(best, sqlite3_column_int64(s, 0));
    });
  }
  sqlite3_close(db);
  return best;
}

int64_t highest_id_under(const fs::path& dir, const fs::path& skip) {
  int64_t best = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".db") continue;
    if (!skip.empty() && fs::equivalent(it->path(), skip, ec)) continue;
    best = std::max(best, highest_id(it->path()));
  }
  return best;
}

}  // namespace goober::ids
