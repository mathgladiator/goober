#include "goober/identity/identity.h"

#include <stdexcept>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/random.h"
#include "goober/sqlite/sqlite.h"
#include "internal.h"

namespace goober::identity {

using namespace detail;

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
  redirect_uris TEXT NOT NULL,           -- JSON array; exact match, except loopback ports (009 §3)
  kind TEXT NOT NULL,                    -- 'public' (PKCE, no secret); 'confidential' isn't offered yet
  secret_hash TEXT,
  created_at INTEGER NOT NULL,
  acts_as TEXT NOT NULL DEFAULT 'agent', -- 'person' or 'agent' (999 decision 1)
  source TEXT NOT NULL DEFAULT 'host'    -- 'host' (the sign-in page), 'config', 'operator', or 'dynamic'
);
CREATE TABLE IF NOT EXISTS authorization_codes (
  code_hash TEXT PRIMARY KEY,
  client_id TEXT NOT NULL REFERENCES clients(client_id),
  user_id INTEGER NOT NULL REFERENCES users(id),
  redirect_uri TEXT NOT NULL,
  code_challenge TEXT NOT NULL,          -- PKCE, S256 only
  expires_at INTEGER NOT NULL,
  consumed_at INTEGER,
  family TEXT                            -- the tokens it was exchanged for, so a replay revokes them
);
CREATE TABLE IF NOT EXISTS device_codes (
  device_code_hash TEXT PRIMARY KEY,
  user_code TEXT NOT NULL UNIQUE,
  client_id TEXT NOT NULL REFERENCES clients(client_id),
  user_id INTEGER REFERENCES users(id),
  expires_at INTEGER NOT NULL,
  approved_at INTEGER,
  denied_at INTEGER,
  polled_at INTEGER,                     -- for slow_down
  consumed_at INTEGER
);
CREATE TABLE IF NOT EXISTS tokens (
  token_hash TEXT PRIMARY KEY,
  kind TEXT NOT NULL,                    -- 'session' (the browser's cookie), 'access', or 'refresh'
  user_id INTEGER NOT NULL REFERENCES users(id),
  client_id TEXT NOT NULL REFERENCES clients(client_id),
  is_agent INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  rotated_to TEXT,
  revoked_at INTEGER,
  family TEXT                            -- one grant: its access and refresh tokens, revoked together
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

// Columns added after a realm's database was first created.
void migrate(sqlite::Db& db) {
  auto has = [&](const char* table, const char* column) {
    sqlite::Stmt s(db, std::string("SELECT count(*) FROM pragma_table_info('") + table + "') WHERE name = ?");
    s.bind(1, std::string_view(column));
    return s.step() && s.int64(0) > 0;
  };
  if (!has("clients", "acts_as")) db.exec("ALTER TABLE clients ADD COLUMN acts_as TEXT NOT NULL DEFAULT 'agent'");
  if (!has("clients", "source")) db.exec("ALTER TABLE clients ADD COLUMN source TEXT NOT NULL DEFAULT 'host'");
  if (!has("tokens", "family")) db.exec("ALTER TABLE tokens ADD COLUMN family TEXT");
  db.exec(
      "INSERT OR IGNORE INTO clients (client_id, name, redirect_uris, kind, secret_hash, created_at, acts_as, source) "
      "VALUES ('goober-sign-in', 'the sign-in page', '[]', 'public', NULL, 0, 'person', 'host')");
  db.exec("UPDATE clients SET acts_as = 'person' WHERE client_id = 'goober-sign-in'");
  db.exec("CREATE INDEX IF NOT EXISTS tokens_by_family ON tokens(family)");
}

}  // namespace

Identity::Realm::Realm(const std::filesystem::path& file) : db(file) {
  db.exec(kSchema);
  migrate(db);
}

bool is_valid_tenant_role(std::string_view role) {
  auto slash = role.find('/');
  if (slash == std::string_view::npos) return is_role_part(role);
  return is_role_part(role.substr(0, slash)) && is_role_part(role.substr(slash + 1));
}

bool is_valid_realm(std::string_view realm) { return realm == kHostRealm || base::is_valid_name(realm); }


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
  // Wrong guesses are counted across codes for a day, so asking for fresh codes doesn't buy
  // fresh guesses: about one chance in fifty thousand a day, from any number of machines.
  sqlite::Stmt failures(r.db, "SELECT coalesce(sum(attempts), 0) FROM email_codes WHERE email = ? AND expires_at > ?");
  failures.bind(1, email).bind(2, now_ms - kFailureWindowMs);
  if (failures.step() && failures.int64(0) >= kFailuresPerDay) {
    return *why = "Too many wrong codes for this address today. Try again tomorrow.", std::nullopt;
  }
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

bj::array Identity::sessions(std::string_view realm_name, int64_t user_id, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db,
                 "SELECT rowid, expires_at FROM tokens WHERE user_id = ? AND kind = 'session' AND revoked_at IS NULL "
                 "AND expires_at > ? ORDER BY expires_at DESC LIMIT 50");
  s.bind(1, user_id).bind(2, now_ms);
  bj::array out;
  while (s.step()) out.push_back(bj::object{{"id", s.int64(0)}, {"expires_at", s.int64(1)}});
  return out;
}

bool Identity::end_session_by_id(std::string_view realm_name, int64_t user_id, int64_t id, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db,
                 "UPDATE tokens SET revoked_at = ? WHERE rowid = ? AND user_id = ? AND kind = 'session' AND "
                 "revoked_at IS NULL");
  s.bind(1, now_ms).bind(2, id).bind(3, user_id).run();
  return r.db.changes() > 0;
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
