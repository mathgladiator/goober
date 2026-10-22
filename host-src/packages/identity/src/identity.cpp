#include "goober/identity/identity.h"

#include <stdexcept>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/random.h"
#include "goober/identity/sqlite.h"

namespace goober::identity {

namespace {

// The 013 schema, v1 subset: no passwords, passkeys, authorization or device codes yet.
constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,  -- AUTOINCREMENT: a user_id is never reused
  email TEXT NOT NULL UNIQUE,
  created_at INTEGER NOT NULL,
  disabled_at INTEGER
);
CREATE TABLE IF NOT EXISTS email_codes (
  id INTEGER PRIMARY KEY,
  email TEXT NOT NULL,
  code_hash TEXT NOT NULL,
  expires_at INTEGER NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  consumed_at INTEGER
);
CREATE INDEX IF NOT EXISTS email_codes_by_email ON email_codes(email, expires_at);
CREATE TABLE IF NOT EXISTS clients (
  client_id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  redirect_uris TEXT NOT NULL,
  kind TEXT NOT NULL,
  secret_hash TEXT,
  created_at INTEGER NOT NULL
);
INSERT OR IGNORE INTO clients VALUES ('goober-sign-in', 'the sign-in page', '[]', 'public', NULL, 0);
CREATE TABLE IF NOT EXISTS tokens (
  token_hash TEXT PRIMARY KEY,
  kind TEXT NOT NULL,                    -- 'session' in v1: the browser's cookie
  user_id INTEGER NOT NULL REFERENCES users(id),
  client_id TEXT NOT NULL REFERENCES clients(client_id),
  is_agent INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  rotated_to TEXT,
  revoked_at INTEGER
);
CREATE INDEX IF NOT EXISTS tokens_by_user ON tokens(user_id);
CREATE TABLE IF NOT EXISTS roles (
  user_id INTEGER NOT NULL REFERENCES users(id),
  role TEXT NOT NULL,
  granted_by INTEGER REFERENCES users(id),
  granted_at INTEGER NOT NULL,
  PRIMARY KEY (user_id, role)
);
CREATE TABLE IF NOT EXISTS audit (
  id INTEGER PRIMARY KEY,
  at INTEGER NOT NULL,
  actor INTEGER,
  action TEXT NOT NULL,
  subject INTEGER,
  detail TEXT NOT NULL
);
)sql";

std::string hash_secret(std::string_view secret) { return "sha256:" + base::sha256_hex(secret); }

std::string code_hash(std::string_view email, std::string_view code) {
  return hash_secret(std::string(email) + "\n" + std::string(code));
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

std::string normalize_email(std::string_view email) {
  std::string e = base::to_lower(email);
  while (!e.empty() && (e.back() == ' ' || e.back() == '\t')) e.pop_back();
  size_t i = 0;
  while (i < e.size() && (e[i] == ' ' || e[i] == '\t')) ++i;
  return e.substr(i);
}

bool looks_like_email(std::string_view e) {
  auto at = e.find('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= e.size() || e.size() > 254) return false;
  for (unsigned char c : e) {
    if (c <= 0x20 || c == '<' || c == '>' || c == '"' || c == ',' || c >= 0x7f) return false;
  }
  return e.find('@', at + 1) == std::string_view::npos;
}

bool is_role_part(std::string_view s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

void audit(sqlite::Db& db, int64_t at, std::optional<int64_t> actor, std::string_view action,
           std::optional<int64_t> subject, const bj::object& detail) {
  sqlite::Stmt s(db, "INSERT INTO audit (at, actor, action, subject, detail) VALUES (?, ?, ?, ?, ?)");
  s.bind(1, at);
  actor ? s.bind(2, *actor) : s.bind_null(2);
  s.bind(3, action);
  subject ? s.bind(4, *subject) : s.bind_null(4);
  s.bind(5, json::dump(detail));
  s.run();
}

std::vector<std::string> roles_of(sqlite::Db& db, int64_t user_id) {
  sqlite::Stmt s(db, "SELECT role FROM roles WHERE user_id = ? ORDER BY role");
  s.bind(1, user_id);
  std::vector<std::string> out;
  while (s.step()) out.push_back(s.text(0));
  return out;
}

std::optional<int64_t> user_id_for(sqlite::Db& db, std::string_view email) {
  sqlite::Stmt s(db, "SELECT id FROM users WHERE email = ?");
  s.bind(1, email);
  if (!s.step()) return std::nullopt;
  return s.int64(0);
}

int64_t create_user(sqlite::Db& db, std::string_view email, int64_t now_ms) {
  sqlite::Stmt s(db, "INSERT INTO users (email, created_at) VALUES (?, ?)");
  s.bind(1, email).bind(2, now_ms).run();
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

}  // namespace

bool is_valid_tenant_role(std::string_view role) {
  auto slash = role.find('/');
  if (slash == std::string_view::npos) return is_role_part(role);
  return is_role_part(role.substr(0, slash)) && is_role_part(role.substr(slash + 1));
}

bool is_valid_realm(std::string_view realm) { return realm == kHostRealm || base::is_valid_name(realm); }

struct Identity::Realm {
  explicit Realm(const std::filesystem::path& file) : db(file) { db.exec(kSchema); }
  std::mutex mu;
  sqlite::Db db;
};

Identity::Identity(std::filesystem::path directory, Mailer mailer)
    : directory_(std::move(directory)), mailer_(std::move(mailer)) {}

Identity::~Identity() = default;

Identity::Realm& Identity::realm(std::string_view name) {
  if (!is_valid_realm(name)) throw std::invalid_argument("not a realm: " + std::string(name));
  std::lock_guard lock(mu_);
  auto it = realms_.find(name);
  if (it != realms_.end()) return *it->second;
  std::filesystem::create_directories(directory_);
  auto r = std::make_unique<Realm>(directory_ / (std::string(name) + ".db"));
  auto& ref = *r;
  realms_.emplace(std::string(name), std::move(r));
  return ref;
}

void Identity::start_email(std::string_view realm_name, std::string_view email_in, std::string_view where,
                           bool signup, int64_t now_ms) {
  std::string email = normalize_email(email_in);
  if (!looks_like_email(email)) return;
  auto& r = realm(realm_name);
  std::string code;
  {
    std::lock_guard lock(r.mu);
    if (!signup && !user_id_for(r.db, email)) return;
    sqlite::Stmt recent(r.db, "SELECT count(*) FROM email_codes WHERE email = ? AND expires_at > ?");
    recent.bind(1, email).bind(2, now_ms);
    if (recent.step() && recent.int64(0) >= kCodesPerWindow) return;
    char digits[8];
    std::snprintf(digits, sizeof(digits), "%06llu", static_cast<unsigned long long>(base::os_random_u64() % 1000000));
    code = digits;
    sqlite::Stmt s(r.db, "INSERT INTO email_codes (email, code_hash, expires_at) VALUES (?, ?, ?)");
    s.bind(1, email).bind(2, code_hash(email, code)).bind(3, now_ms + kCodeTtlMs).run();
  }
  mailer_(Mail{email, "Your sign-in code for " + std::string(where),
               "Your sign-in code for " + std::string(where) + " is " + code +
                   ".\n\nIt was asked for by the sign-in page, and it expires in 10 minutes.\n"
                   "Type it into the sign-in page you already have open. Never share this code with anyone.\n"});
}

std::optional<SignIn> Identity::verify_email(std::string_view realm_name, std::string_view email_in,
                                             std::string_view code, bool signup, int64_t now_ms,
                                             int64_t session_ms, std::string* why) {
  const std::string kNope = "That code didn't work. Check it, or ask for a new one.";
  std::string email = normalize_email(email_in);
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt find(r.db,
                    "SELECT id, code_hash, attempts FROM email_codes WHERE email = ? AND consumed_at IS NULL "
                    "AND expires_at > ? ORDER BY id DESC LIMIT 1");
  find.bind(1, email).bind(2, now_ms);
  if (!find.step()) return *why = kNope, std::nullopt;
  int64_t code_id = find.int64(0);
  std::string stored = find.text(1);
  if (find.int64(2) >= kCodeMaxAttempts) return *why = "Too many tries. Ask for a new code.", std::nullopt;
  if (!constant_time_equal(stored, code_hash(email, code))) {
    sqlite::Stmt bump(r.db, "UPDATE email_codes SET attempts = attempts + 1 WHERE id = ?");
    bump.bind(1, code_id).run();
    tx.commit();
    return *why = kNope, std::nullopt;
  }
  sqlite::Stmt consume(r.db, "UPDATE email_codes SET consumed_at = ? WHERE id = ?");
  consume.bind(1, now_ms).bind(2, code_id).run();

  SignIn out;
  auto id = user_id_for(r.db, email);
  if (!id) {
    if (!signup) {
      tx.commit();
      return *why = kNope, std::nullopt;
    }
    id = create_user(r.db, email, now_ms);
    out.created = true;
    audit(r.db, now_ms, *id, "user_created", *id, bj::object{{"email", email}});
  } else {
    sqlite::Stmt disabled(r.db, "SELECT disabled_at FROM users WHERE id = ?");
    disabled.bind(1, *id);
    if (disabled.step() && !disabled.is_null(0)) {
      tx.commit();
      return *why = kNope, std::nullopt;
    }
  }
  uint8_t raw[32];
  base::os_random_fill(raw);
  out.token = base::hex_encode(std::string_view(reinterpret_cast<char*>(raw), sizeof(raw)));
  sqlite::Stmt tok(r.db,
                   "INSERT INTO tokens (token_hash, kind, user_id, client_id, is_agent, expires_at) "
                   "VALUES (?, 'session', ?, 'goober-sign-in', 0, ?)");
  tok.bind(1, hash_secret(out.token)).bind(2, *id).bind(3, now_ms + session_ms).run();
  audit(r.db, now_ms, *id, "sign_in", *id, bj::object{{"method", "email_code"}});
  out.user = User{*id, email, std::string(realm_name), roles_of(r.db, *id)};
  tx.commit();
  return out;
}

std::optional<User> Identity::session(std::string_view realm_name, std::string_view token, int64_t now_ms) {
  if (token.empty()) return std::nullopt;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db,
                 "SELECT u.id, u.email FROM tokens t JOIN users u ON u.id = t.user_id WHERE t.token_hash = ? "
                 "AND t.kind = 'session' AND t.revoked_at IS NULL AND t.expires_at > ? AND u.disabled_at IS NULL");
  s.bind(1, hash_secret(token)).bind(2, now_ms);
  if (!s.step()) return std::nullopt;
  User u{s.int64(0), s.text(1), std::string(realm_name), {}};
  u.roles = roles_of(r.db, u.user_id);
  return u;
}

void Identity::end_session(std::string_view realm_name, std::string_view token, int64_t now_ms) {
  if (token.empty()) return;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "UPDATE tokens SET revoked_at = ? WHERE token_hash = ? AND revoked_at IS NULL");
  s.bind(1, now_ms).bind(2, hash_secret(token)).run();
}

bool Identity::grant(std::string_view realm_name, std::string_view email_in, std::string_view role,
                     std::string_view by, int64_t now_ms, std::string* why) {
  std::string email = normalize_email(email_in);
  if (!looks_like_email(email)) return *why = "not an email address", false;
  if (realm_name == kHostRealm) return *why = "host roles come from identity.operators in the host config", false;
  if (!is_valid_tenant_role(role)) return *why = "a role is role or role/scope, each [A-Za-z0-9_]+", false;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  auto id = user_id_for(r.db, email);
  if (!id) {
    id = create_user(r.db, email, now_ms);
    audit(r.db, now_ms, std::nullopt, "user_created", *id, bj::object{{"email", email}, {"by", by}});
  }
  sqlite::Stmt s(r.db, "INSERT OR IGNORE INTO roles (user_id, role, granted_by, granted_at) VALUES (?, ?, NULL, ?)");
  s.bind(1, *id).bind(2, role).bind(3, now_ms).run();
  if (r.db.changes() > 0) audit(r.db, now_ms, std::nullopt, "grant", *id, bj::object{{"role", role}, {"by", by}});
  tx.commit();
  return true;
}

bool Identity::revoke(std::string_view realm_name, std::string_view email_in, std::string_view role,
                      std::string_view by, int64_t now_ms, std::string* why) {
  std::string email = normalize_email(email_in);
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  auto id = user_id_for(r.db, email);
  if (!id) return *why = "no such user", false;
  sqlite::Stmt s(r.db, "DELETE FROM roles WHERE user_id = ? AND role = ?");
  s.bind(1, *id).bind(2, role).run();
  if (r.db.changes() == 0) return *why = "the user doesn't hold that role", false;
  audit(r.db, now_ms, std::nullopt, "revoke", *id, bj::object{{"role", role}, {"by", by}});
  tx.commit();
  return true;
}

int Identity::revoke_sessions(std::string_view realm_name, std::string_view email_in, std::string_view by,
                              int64_t now_ms) {
  std::string email = normalize_email(email_in);
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  auto id = user_id_for(r.db, email);
  if (!id) return 0;
  sqlite::Stmt s(r.db, "UPDATE tokens SET revoked_at = ? WHERE user_id = ? AND revoked_at IS NULL");
  s.bind(1, now_ms).bind(2, *id).run();
  int n = r.db.changes();
  audit(r.db, now_ms, std::nullopt, "token_revoked", *id, bj::object{{"count", n}, {"by", by}});
  tx.commit();
  return n;
}

int Identity::holders(std::string_view realm_name, std::string_view role) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "SELECT count(*) FROM roles WHERE role = ?");
  s.bind(1, role);
  return s.step() ? int(s.int64(0)) : 0;
}

bj::value Identity::describe(std::string_view realm_name, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  bj::array users;
  sqlite::Stmt s(r.db,
                 "SELECT u.id, u.email, u.created_at, u.disabled_at, (SELECT count(*) FROM tokens t WHERE "
                 "t.user_id = u.id AND t.revoked_at IS NULL AND t.expires_at > ?) FROM users u ORDER BY u.id");
  s.bind(1, now_ms);
  while (s.step()) {
    bj::array roles;
    for (auto& role : roles_of(r.db, s.int64(0))) roles.push_back(bj::value(role));
    users.push_back(bj::object{{"user_id", std::to_string(s.int64(0))},
                               {"email", s.text(1)},
                               {"created_at", s.int64(2)},
                               {"disabled", !s.is_null(3)},
                               {"roles", std::move(roles)},
                               {"live_sessions", s.int64(4)}});
  }
  bj::array recent;
  sqlite::Stmt a(r.db, "SELECT at, actor, action, subject, detail FROM audit ORDER BY id DESC LIMIT 20");
  while (a.step()) {
    std::string err;
    auto detail = json::parse(a.text(4), &err);
    recent.push_back(bj::object{{"at", a.int64(0)},
                                {"actor", a.is_null(1) ? bj::value(nullptr) : bj::value(std::to_string(a.int64(1)))},
                                {"action", a.text(2)},
                                {"subject", a.is_null(3) ? bj::value(nullptr) : bj::value(std::to_string(a.int64(3)))},
                                {"detail", detail ? *detail : bj::value(nullptr)}});
  }
  return bj::object{{"realm", realm_name}, {"users", std::move(users)}, {"audit", std::move(recent)}};
}

}  // namespace goober::identity
