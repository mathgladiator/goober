#pragma once

// Pieces shared by identity.cpp (sign-in, sessions, roles) and oauth.cpp (clients,
// codes, and tokens). Not part of the package's interface.

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/identity/identity.h"
#include "goober/json/json.h"
#include "goober/sqlite/sqlite.h"

namespace goober::identity {

struct Identity::Realm {
  explicit Realm(const std::filesystem::path& file);
  std::mutex mu;
  sqlite::Db db;
};

namespace detail {

inline std::string hash_secret(std::string_view secret) { return "sha256:" + base::sha256_hex(secret); }

inline std::string code_hash(std::string_view email, std::string_view code) {
  return hash_secret(std::string(email) + "\n" + std::string(code));
}

inline bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

inline std::string normalize_email(std::string_view email) {
  std::string e = base::to_lower(email);
  while (!e.empty() && (e.back() == ' ' || e.back() == '\t')) e.pop_back();
  size_t i = 0;
  while (i < e.size() && (e[i] == ' ' || e[i] == '\t')) ++i;
  return e.substr(i);
}

inline bool looks_like_email(std::string_view e) {
  auto at = e.find('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= e.size() || e.size() > 254) return false;
  for (unsigned char c : e) {
    if (c <= 0x20 || c == '<' || c == '>' || c == '"' || c == ',' || c >= 0x7f) return false;
  }
  return e.find('@', at + 1) == std::string_view::npos;
}

inline bool is_role_part(std::string_view s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

inline void audit(sqlite::Db& db, int64_t at, std::optional<int64_t> actor, std::string_view action,
           std::optional<int64_t> subject, const bj::object& detail) {
  sqlite::Stmt s(db, "INSERT INTO audit (at, actor, action, subject, detail) VALUES (?, ?, ?, ?, ?)");
  s.bind(1, at);
  actor ? s.bind(2, *actor) : s.bind_null(2);
  s.bind(3, action);
  subject ? s.bind(4, *subject) : s.bind_null(4);
  s.bind(5, json::dump(detail));
  s.run();
}

inline std::vector<std::string> roles_of(sqlite::Db& db, int64_t user_id) {
  sqlite::Stmt s(db, "SELECT role FROM roles WHERE user_id = ? ORDER BY role");
  s.bind(1, user_id);
  std::vector<std::string> out;
  while (s.step()) out.push_back(s.text(0));
  return out;
}

inline std::optional<int64_t> user_id_for(sqlite::Db& db, std::string_view email) {
  sqlite::Stmt s(db, "SELECT id FROM users WHERE email = ?");
  s.bind(1, email);
  if (!s.step()) return std::nullopt;
  return s.int64(0);
}

// `id` comes from the host's id service (038 § 2); 0 lets AUTOINCREMENT number the user.
inline int64_t create_user(sqlite::Db& db, std::string_view email, int64_t now_ms, int64_t id = 0) {
  sqlite::Stmt s(db, "INSERT INTO users (id, email, created_at) VALUES (?, ?, ?)");
  if (id > 0) s.bind(1, id);
  else s.bind_null(1);
  s.bind(2, email).bind(3, now_ms).run();
  return db.last_insert_rowid();
}

// BEGIN IMMEDIATE ... COMMIT, rolled back if anything throws.
class Transaction {
 public:
  explicit Transaction(sqlite::Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
  ~Transaction() {
    if (!done_) {
      try {
        db_.exec("ROLLBACK");
      } catch (...) {
      }
    }
  }
  void commit() {
    db_.exec("COMMIT");
    done_ = true;
  }

 private:
  sqlite::Db& db_;
  bool done_ = false;
};


}  // namespace detail
}  // namespace goober::identity
