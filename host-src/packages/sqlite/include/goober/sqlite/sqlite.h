#pragma once

// A small RAII wrapper over SQLite: one connection, prepared statements, and
// typed binds and columns. Errors throw std::runtime_error with SQLite's message.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace goober::sqlite {

class Db {
 public:
  explicit Db(const std::filesystem::path& file);
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  void exec(std::string_view sql);
  int64_t last_insert_rowid() const;
  int changes() const;
  sqlite3* raw() const { return db_; }

 private:
  sqlite3* db_ = nullptr;
};

class Stmt {
 public:
  Stmt(Db& db, std::string_view sql);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  // Binds are 1-based, like SQLite's.
  Stmt& bind(int index, int64_t value);
  Stmt& bind(int index, std::string_view value);
  Stmt& bind_null(int index);
  // Bytes as a BLOB, whatever they hold.
  Stmt& bind_blob(int index, std::string_view bytes);

  // Steps once: true when a row is ready, false when done.
  bool step();
  // Steps until done, for statements that return no rows.
  void run();

  int64_t int64(int column) const;
  std::string text(int column) const;
  std::string blob(int column) const;
  bool is_null(int column) const;

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace goober::sqlite
