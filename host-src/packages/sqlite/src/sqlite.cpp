#include "goober/sqlite/sqlite.h"

#include <sqlite3.h>

#include <stdexcept>

namespace goober::sqlite {

namespace {
[[noreturn]] void fail(sqlite3* db, std::string_view what) {
  throw std::runtime_error(std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "out of memory"));
}
}  // namespace

Db::Db(const std::filesystem::path& file) {
  if (sqlite3_open_v2(file.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                      nullptr) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    throw std::runtime_error("cannot open " + file.string() + ": " + msg);
  }
  sqlite3_busy_timeout(db_, 5000);
  exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
}

Db::~Db() { sqlite3_close(db_); }

void Db::exec(std::string_view sql) {
  char* msg = nullptr;
  if (sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
    std::string text = msg ? msg : "unknown error";
    sqlite3_free(msg);
    throw std::runtime_error("sqlite: " + text);
  }
}

int64_t Db::last_insert_rowid() const { return sqlite3_last_insert_rowid(db_); }
int Db::changes() const { return sqlite3_changes(db_); }

Stmt::Stmt(Db& db, std::string_view sql) : db_(db.raw()) {
  if (sqlite3_prepare_v2(db_, sql.data(), int(sql.size()), &stmt_, nullptr) != SQLITE_OK) fail(db_, "prepare");
}

Stmt::~Stmt() { sqlite3_finalize(stmt_); }

Stmt& Stmt::bind(int index, int64_t value) {
  if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) fail(db_, "bind");
  return *this;
}

Stmt& Stmt::bind(int index, std::string_view value) {
  if (sqlite3_bind_text(stmt_, index, value.data(), int(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    fail(db_, "bind");
  }
  return *this;
}

Stmt& Stmt::bind_null(int index) {
  if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) fail(db_, "bind");
  return *this;
}

bool Stmt::step() {
  int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  fail(db_, "step");
}

void Stmt::run() {
  while (step()) {
  }
}

int64_t Stmt::int64(int column) const { return sqlite3_column_int64(stmt_, column); }

std::string Stmt::text(int column) const {
  const auto* p = sqlite3_column_text(stmt_, column);
  return p ? std::string(reinterpret_cast<const char*>(p), size_t(sqlite3_column_bytes(stmt_, column))) : "";
}

bool Stmt::is_null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

}  // namespace goober::sqlite
