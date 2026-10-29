// OAuth 2.1 for the built-in identity service (013.Identity.md § Sessions, schema, and
// the login API; design/machine/009.OAuth.md): clients that say whether they act as the
// person or as an agent, authorization codes with PKCE (S256 only), rotating refresh
// tokens with reuse detection, revocation by grant, and the device flow for agents.
//
// Every token, code, and device code is stored as its sha256: hash. A grant is a
// "family": the tokens one sign-in produced, revoked together.

#include <stdexcept>

#include "goober/base/encoding.h"
#include "goober/base/random.h"
#include "internal.h"

namespace goober::identity {

using namespace detail;

namespace {

std::string random_bytes(size_t n) {
  std::string out(n, '\0');
  base::os_random_fill(std::span<uint8_t>(reinterpret_cast<uint8_t*>(out.data()), n));
  return out;
}

std::string random_hex(size_t bytes) { return base::hex_encode(random_bytes(bytes)); }

bool is_client_id(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-';
    if (!ok) return false;
  }
  return true;
}

bool is_client_name(std::string_view name) {
  if (name.empty() || name.size() > 80) return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

// The parts of a loopback redirect: http://127.0.0.1[:port]/path or http://[::1][:port]/path.
struct Loopback {
  std::string host;
  std::string rest;  // the path and query, from the first "/"
  bool has_port = false;
};

std::optional<Loopback> parse_loopback(std::string_view uri) {
  constexpr std::string_view kHttp = "http://";
  if (!uri.starts_with(kHttp)) return std::nullopt;
  uri.remove_prefix(kHttp.size());
  Loopback out;
  if (uri.starts_with("127.0.0.1")) {
    out.host = "127.0.0.1";
  } else if (uri.starts_with("[::1]")) {
    out.host = "[::1]";
  } else {
    return std::nullopt;
  }
  uri.remove_prefix(out.host.size());
  if (!uri.empty() && uri[0] == ':') {
    uri.remove_prefix(1);
    size_t digits = 0;
    while (digits < uri.size() && uri[digits] >= '0' && uri[digits] <= '9') ++digits;
    if (digits == 0 || digits > 5) return std::nullopt;
    if (std::stoi(std::string(uri.substr(0, digits))) > 65535) return std::nullopt;
    uri.remove_prefix(digits);
    out.has_port = true;
  }
  if (uri.empty() || uri[0] != '/') return std::nullopt;
  out.rest = std::string(uri);
  return out;
}

std::vector<std::string> parse_uris(const std::string& text) {
  std::vector<std::string> out;
  std::string err;
  auto v = json::parse(text, &err);
  if (!v || !v->is_array()) return out;
  for (const auto& u : v->as_array()) {
    if (u.is_string()) out.emplace_back(u.as_string());
  }
  return out;
}

std::string dump_uris(const std::vector<std::string>& uris) {
  bj::array a;
  for (const auto& u : uris) a.push_back(bj::value(u));
  return json::dump(a);
}

Client read_client(sqlite::Stmt& s) {
  // Columns: client_id, name, redirect_uris, acts_as, source, created_at.
  Client c;
  c.client_id = s.text(0);
  c.name = s.text(1);
  c.redirect_uris = parse_uris(s.text(2));
  c.person = s.text(3) == "person";
  c.source = s.text(4);
  c.created_at = s.int64(5);
  return c;
}

constexpr const char* kClientColumns = "client_id, name, redirect_uris, acts_as, source, created_at";

std::optional<Client> find_client(sqlite::Db& db, std::string_view client_id) {
  sqlite::Stmt s(db, std::string("SELECT ") + kClientColumns + " FROM clients WHERE client_id = ?");
  s.bind(1, client_id);
  if (!s.step()) return std::nullopt;
  return read_client(s);
}

bool user_active(sqlite::Db& db, int64_t user_id) {
  sqlite::Stmt s(db, "SELECT disabled_at FROM users WHERE id = ?");
  s.bind(1, user_id);
  return s.step() && s.is_null(0);
}

int revoke_family(sqlite::Db& db, std::string_view family, int64_t now_ms) {
  if (family.empty()) return 0;
  sqlite::Stmt s(db, "UPDATE tokens SET revoked_at = ? WHERE family = ? AND revoked_at IS NULL");
  s.bind(1, now_ms).bind(2, family).run();
  return db.changes();
}

// The S256 challenge of a verifier: base64url(sha256(verifier)), no padding.
std::string s256(std::string_view verifier) {
  auto digest = base::sha256(verifier);
  return base::base64url_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

// RFC 7636 §4.1: 43 to 128 of [A-Za-z0-9-._~].
bool is_code_verifier(std::string_view v) {
  if (v.size() < 43 || v.size() > 128) return false;
  for (char c : v) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
              c == '_' || c == '~';
    if (!ok) return false;
  }
  return true;
}

// User codes avoid vowels and look-alikes (RFC 8628 §6.1), shown as XXXX-XXXX.
constexpr std::string_view kUserCodeAlphabet = "BCDFGHJKLMNPQRSTVWXZ";

std::string new_user_code() {
  std::string out;
  for (int i = 0; i < 8; ++i) {
    if (i == 4) out.push_back('-');
    out.push_back(kUserCodeAlphabet[base::os_random_u64() % kUserCodeAlphabet.size()]);
  }
  return out;
}

// Upper case, dashes and spaces removed, then the dash put back: "wdjb mjht" is WDJB-MJHT.
std::string normalize_user_code(std::string_view in) {
  std::string letters;
  for (char c : in) {
    if (c == '-' || c == ' ') continue;
    letters.push_back(c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c);
  }
  if (letters.size() != 8) return "";
  return letters.substr(0, 4) + "-" + letters.substr(4);
}


}  // namespace

bool is_valid_redirect_uri(std::string_view uri) {
  if (uri.empty() || uri.size() > 512 || uri.find('#') != std::string_view::npos) return false;
  for (unsigned char c : uri) {
    if (c <= 0x20 || c >= 0x7f) return false;
  }
  if (parse_loopback(uri)) return true;
  // MCP clients register http://localhost:PORT/callback. It is allowed, but matched exactly:
  // only the IP literals get the any-port rule (999 decision 2).
  if (uri.starts_with("http://localhost:") || uri.starts_with("http://localhost/")) {
    auto rest = uri.substr(16);
    size_t i = 0;
    if (!rest.empty() && rest[0] == ':') {
      for (i = 1; i < rest.size() && rest[i] >= '0' && rest[i] <= '9'; ++i) {
      }
      if (i == 1 || i > 6) return false;
    }
    return i < rest.size() && rest[i] == '/';
  }
  if (uri.starts_with("https://")) {
    auto host = uri.substr(8);
    return !host.empty() && host[0] != '/' && host.find('/') != std::string_view::npos;
  }
  // A private-use scheme (RFC 8252 §7.1), which should be reverse-DNS (com.example.app:). Some
  // clients use one word (Cursor sends cursor://), so any scheme is taken but the web's own and
  // the ones a browser runs or reads itself.
  auto colon = uri.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  auto scheme = uri.substr(0, colon);
  static constexpr std::string_view kRefused[] = {"http",  "https", "javascript", "vbscript", "data",   "file",
                                                  "about", "blob",  "filesystem", "ftp",      "ws",     "wss",
                                                  "mailto", "tel",  "sms",        "intent",   "chrome", "view-source"};
  for (auto r : kRefused) {
    if (scheme == r) return false;
  }
  if (!(scheme[0] >= 'a' && scheme[0] <= 'z')) return false;
  for (char c : scheme) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+';
    if (!ok) return false;
  }
  return colon + 1 < uri.size();
}

bool redirect_allowed(const Client& client, std::string_view redirect_uri) {
  auto asked = parse_loopback(redirect_uri);
  for (const auto& registered : client.redirect_uris) {
    if (registered == redirect_uri) return true;
    if (!asked) continue;
    // Loopback: the port may differ, nothing else may (RFC 8252 §7.3).
    auto reg = parse_loopback(registered);
    if (reg && reg->host == asked->host && reg->rest == asked->rest) return true;
  }
  return false;
}

bool Identity::put_client(std::string_view realm_name, const Client& c, int64_t now_ms, std::string* why) {
  if (!is_client_id(c.client_id)) return *why = "a client_id is 1 to 64 of [A-Za-z0-9._-]", false;
  if (c.client_id == "goober-sign-in") return *why = "goober-sign-in belongs to the host", false;
  if (!is_client_name(c.name)) return *why = "a client name is 1 to 80 printable characters", false;
  // An agent may have none, and use only the device flow; a person client always signs in through a redirect.
  if (c.redirect_uris.size() > 10) return *why = "a client has at most 10 redirect URIs", false;
  if (c.person && c.redirect_uris.empty()) return *why = "a person client needs a redirect URI", false;
  for (const auto& u : c.redirect_uris) {
    if (!is_valid_redirect_uri(u)) return *why = "not an allowed redirect URI: " + u, false;
  }
  std::string source = c.source.empty() ? "operator" : c.source;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt s(r.db,
                 "INSERT INTO clients (client_id, name, redirect_uris, kind, secret_hash, created_at, acts_as, source) "
                 "VALUES (?, ?, ?, 'public', NULL, ?, ?, ?) ON CONFLICT(client_id) DO UPDATE SET name = excluded.name, "
                 "redirect_uris = excluded.redirect_uris, acts_as = excluded.acts_as, source = excluded.source");
  s.bind(1, c.client_id)
      .bind(2, c.name)
      .bind(3, dump_uris(c.redirect_uris))
      .bind(4, now_ms)
      .bind(5, std::string_view(c.person ? "person" : "agent"))
      .bind(6, source)
      .run();
  audit(r.db, now_ms, std::nullopt, "client_put", std::nullopt,
        bj::object{{"client_id", c.client_id}, {"acts_as", c.person ? "person" : "agent"}, {"source", source}});
  tx.commit();
  return true;
}

int Identity::prune(std::string_view realm_name, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  constexpr int64_t kDay = 24LL * 3600 * 1000;
  // Codes long past use, and agents that registered a week ago and never signed anyone in.
  sqlite::Stmt codes(r.db, "DELETE FROM authorization_codes WHERE expires_at < ?");
  codes.bind(1, now_ms - kDay).run();
  sqlite::Stmt devices(r.db, "DELETE FROM device_codes WHERE expires_at < ?");
  devices.bind(1, now_ms - kDay).run();
  sqlite::Stmt email(r.db, "DELETE FROM email_codes WHERE expires_at < ?");
  email.bind(1, now_ms - kDay).run();
  sqlite::Stmt idle(r.db,
                    "DELETE FROM clients WHERE source = 'dynamic' AND created_at < ? AND client_id NOT IN "
                    "(SELECT client_id FROM tokens) AND client_id NOT IN (SELECT client_id FROM authorization_codes) "
                    "AND client_id NOT IN (SELECT client_id FROM device_codes)");
  idle.bind(1, now_ms - 7 * kDay).run();
  sqlite::Stmt events(r.db, "DELETE FROM events WHERE at < ?");
  events.bind(1, now_ms - kEventTtlMs).run();
  // Invites never taken up: the address was never proved, so the account doesn't stay.
  Transaction tx(r.db);
  std::vector<std::pair<int64_t, std::string>> stale;
  sqlite::Stmt find(r.db, "SELECT id, email FROM users WHERE verified_at IS NULL AND created_at < ?");
  find.bind(1, now_ms - kUnverifiedTtlMs);
  while (find.step()) stale.emplace_back(find.int64(0), find.text(1));
  for (const auto& [id, email] : stale) {
    for (const char* sql : {"DELETE FROM roles WHERE user_id = ?", "UPDATE roles SET granted_by = NULL WHERE granted_by = ?",
                            "DELETE FROM tokens WHERE user_id = ?", "DELETE FROM authorization_codes WHERE user_id = ?",
                            "DELETE FROM device_codes WHERE user_id = ?", "DELETE FROM users WHERE id = ?"}) {
      sqlite::Stmt d(r.db, sql);
      d.bind(1, id).run();
    }
    audit(r.db, now_ms, std::nullopt, "user_expired", id, bj::object{{"email", email}, {"why", "never verified"}});
  }
  tx.commit();
  return int(stale.size());
}

std::vector<Event> Identity::events_after(std::string_view realm_name, int64_t after_id, int limit) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  std::vector<Event> out;
  sqlite::Stmt s(r.db, "SELECT id, kind, user_id, email, at FROM events WHERE id > ? ORDER BY id LIMIT ?");
  s.bind(1, after_id).bind(2, int64_t(limit));
  while (s.step()) out.push_back(Event{s.int64(0), s.text(1), s.int64(2), s.text(3), s.int64(4)});
  return out;
}

int64_t Identity::last_event_id(std::string_view realm_name) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  // The sequence, not max(id): pruned events leave it where it was.
  sqlite::Stmt s(r.db, "SELECT coalesce((SELECT seq FROM sqlite_sequence WHERE name = 'events'), 0)");
  return s.step() ? s.int64(0) : 0;
}

std::optional<Client> Identity::register_client(std::string_view realm_name, std::string_view name,
                                                const std::vector<std::string>& redirect_uris, int64_t now_ms,
                                                std::string* why) {
  prune(realm_name, now_ms);
  Client c;
  c.client_id = "dyn_" + random_hex(12);
  c.name = std::string(name);
  c.redirect_uris = redirect_uris;
  c.person = false;  // dynamically registered clients are always agents (999 decision 1)
  c.source = "dynamic";
  c.created_at = now_ms;
  if (!put_client(realm_name, c, now_ms, why)) return std::nullopt;
  return c;
}

std::optional<Client> Identity::client(std::string_view realm_name, std::string_view client_id) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  return find_client(r.db, client_id);
}

std::vector<Client> Identity::clients(std::string_view realm_name) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, std::string("SELECT ") + kClientColumns +
                           " FROM clients WHERE client_id != 'goober-sign-in' ORDER BY client_id");
  std::vector<Client> out;
  while (s.step()) out.push_back(read_client(s));
  return out;
}

bool Identity::remove_client(std::string_view realm_name, std::string_view client_id, int64_t now_ms) {
  if (client_id == "goober-sign-in") return false;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt revoke(r.db, "UPDATE tokens SET revoked_at = ? WHERE client_id = ? AND revoked_at IS NULL");
  revoke.bind(1, now_ms).bind(2, client_id).run();
  int tokens = r.db.changes();
  // Its codes and tokens go with it; the audit keeps the record.
  for (const char* table : {"tokens", "authorization_codes", "device_codes"}) {
    sqlite::Stmt gone(r.db, std::string("DELETE FROM ") + table + " WHERE client_id = ?");
    gone.bind(1, client_id).run();
  }
  sqlite::Stmt del(r.db, "DELETE FROM clients WHERE client_id = ?");
  del.bind(1, client_id).run();
  bool removed = r.db.changes() > 0;
  if (removed) {
    audit(r.db, now_ms, std::nullopt, "client_removed", std::nullopt,
          bj::object{{"client_id", client_id}, {"tokens_revoked", tokens}});
  }
  tx.commit();
  return removed;
}

std::string Identity::issue_code(std::string_view realm_name, std::string_view client_id, int64_t user_id,
                                 std::string_view redirect_uri, std::string_view code_challenge, int64_t now_ms) {
  std::string code = base::base64url_encode(random_bytes(32));
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db,
                 "INSERT INTO authorization_codes (code_hash, client_id, user_id, redirect_uri, code_challenge, "
                 "expires_at) VALUES (?, ?, ?, ?, ?, ?)");
  s.bind(1, hash_secret(code))
      .bind(2, client_id)
      .bind(3, user_id)
      .bind(4, redirect_uri)
      .bind(5, code_challenge)
      .bind(6, now_ms + kAuthorizationCodeTtlMs)
      .run();
  return code;
}

std::optional<Tokens> Identity::issue_tokens(Realm& r, std::string_view client_id, int64_t user_id, bool agent,
                                             std::string family, int64_t now_ms, int64_t refresh_ms) {
  Tokens t;
  t.access_token = "goa_" + random_hex(32);
  t.refresh_token = "gor_" + random_hex(32);
  t.expires_in_s = kAccessTokenTtlMs / 1000;
  sqlite::Stmt ins(r.db,
                   "INSERT INTO tokens (token_hash, kind, user_id, client_id, is_agent, expires_at, family) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?)");
  ins.bind(1, hash_secret(t.access_token))
      .bind(2, std::string_view("access"))
      .bind(3, user_id)
      .bind(4, client_id)
      .bind(5, int64_t(agent ? 1 : 0))
      .bind(6, now_ms + kAccessTokenTtlMs)
      .bind(7, family)
      .run();
  sqlite::Stmt ref(r.db,
                   "INSERT INTO tokens (token_hash, kind, user_id, client_id, is_agent, expires_at, family) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?)");
  ref.bind(1, hash_secret(t.refresh_token))
      .bind(2, std::string_view("refresh"))
      .bind(3, user_id)
      .bind(4, client_id)
      .bind(5, int64_t(agent ? 1 : 0))
      .bind(6, now_ms + refresh_ms)
      .bind(7, family)
      .run();
  return t;
}

std::optional<Tokens> Identity::exchange_code(std::string_view realm_name, std::string_view client_id,
                                              std::string_view code, std::string_view redirect_uri,
                                              std::string_view code_verifier, int64_t now_ms, int64_t refresh_ms,
                                              OAuthError* error) {
  auto fail = [&](const char* code_name, const char* description) {
    *error = OAuthError{code_name, description};
    return std::nullopt;
  };
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt find(r.db,
                    "SELECT client_id, user_id, redirect_uri, code_challenge, expires_at, consumed_at, family "
                    "FROM authorization_codes WHERE code_hash = ?");
  find.bind(1, hash_secret(code));
  if (!find.step()) return fail("invalid_grant", "unknown authorization code");
  std::string code_client = find.text(0);
  int64_t user_id = find.int64(1);
  std::string code_redirect = find.text(2);
  std::string challenge = find.text(3);
  int64_t expires = find.int64(4);
  bool consumed = !find.is_null(5);
  std::string old_family = find.is_null(6) ? "" : find.text(6);
  if (consumed) {
    // A replayed code: whoever holds it isn't the client that used it, so its tokens go too.
    int n = revoke_family(r.db, old_family, now_ms);
    audit(r.db, now_ms, std::nullopt, "code_replayed", user_id,
          bj::object{{"client_id", code_client}, {"tokens_revoked", n}});
    tx.commit();
    return fail("invalid_grant", "authorization code already used");
  }
  if (expires <= now_ms) return fail("invalid_grant", "authorization code expired");
  if (code_client != client_id) return fail("invalid_grant", "authorization code issued to another client");
  if (code_redirect != redirect_uri) return fail("invalid_grant", "redirect_uri doesn't match the authorization");
  if (!is_code_verifier(code_verifier) || !constant_time_equal(s256(code_verifier), challenge)) {
    return fail("invalid_grant", "PKCE verification failed");
  }
  auto c = find_client(r.db, client_id);
  if (!c) return fail("invalid_client", "unknown client");
  if (!user_active(r.db, user_id)) return fail("invalid_grant", "the account is disabled");
  std::string family = random_hex(16);
  sqlite::Stmt consume(r.db, "UPDATE authorization_codes SET consumed_at = ?, family = ? WHERE code_hash = ?");
  consume.bind(1, now_ms).bind(2, family).bind(3, hash_secret(code)).run();
  auto t = issue_tokens(r, client_id, user_id, !c->person, family, now_ms, refresh_ms);
  audit(r.db, now_ms, user_id, "token_issued",
        user_id, bj::object{{"client_id", client_id}, {"grant", "authorization_code"}, {"agent", !c->person}});
  tx.commit();
  return t;
}

std::optional<Tokens> Identity::refresh(std::string_view realm_name, std::string_view client_id,
                                        std::string_view refresh_token, int64_t now_ms, int64_t refresh_ms,
                                        OAuthError* error) {
  auto fail = [&](const char* code_name, const char* description) {
    *error = OAuthError{code_name, description};
    return std::nullopt;
  };
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  std::string hash = hash_secret(refresh_token);
  sqlite::Stmt find(r.db,
                    "SELECT client_id, user_id, is_agent, expires_at, rotated_to, revoked_at, family, rotated_at FROM tokens "
                    "WHERE token_hash = ? AND kind = 'refresh'");
  find.bind(1, hash);
  if (!find.step()) return fail("invalid_grant", "unknown refresh token");
  std::string token_client = find.text(0);
  int64_t user_id = find.int64(1);
  bool agent = find.int64(2) != 0;
  int64_t expires = find.int64(3);
  bool rotated = !find.is_null(4);
  bool revoked = !find.is_null(5);
  std::string family = find.is_null(6) ? "" : find.text(6);
  int64_t rotated_at = find.is_null(7) ? 0 : find.int64(7);
  if (token_client != client_id) return fail("invalid_grant", "refresh token issued to another client");
  if (revoked) return fail("invalid_grant", "refresh token revoked");
  bool raced = rotated && rotated_at > 0 && now_ms - rotated_at <= kRefreshReuseGraceMs;
  if (rotated && !raced) {
    // Reuse: two parties hold this grant, so neither keeps it.
    int n = revoke_family(r.db, family, now_ms);
    audit(r.db, now_ms, std::nullopt, "refresh_reused", user_id,
          bj::object{{"client_id", client_id}, {"tokens_revoked", n}});
    tx.commit();
    return fail("invalid_grant", "refresh token already used; the grant is revoked");
  }
  if (expires <= now_ms) return fail("invalid_grant", "refresh token expired");
  auto c = find_client(r.db, client_id);
  if (!c) return fail("invalid_client", "unknown client");
  if (!user_active(r.db, user_id)) return fail("invalid_grant", "the account is disabled");
  // What the client is now decides, so turning a person client into an agent takes effect
  // at its next refresh rather than never.
  agent = agent || !c->person;
  auto t = issue_tokens(r, client_id, user_id, agent, family, now_ms, refresh_ms);
  if (raced) {
    // The first rotation stands; this caller gets its own pair in the same grant.
    audit(r.db, now_ms, std::nullopt, "refresh_raced", user_id, bj::object{{"client_id", client_id}});
  } else {
    sqlite::Stmt mark(r.db, "UPDATE tokens SET rotated_to = ?, rotated_at = ? WHERE token_hash = ?");
    mark.bind(1, hash_secret(t->refresh_token)).bind(2, now_ms).bind(3, hash).run();
  }
  // The grant's other access tokens run out on their own (an hour at most): ending them here
  // cut off a second session sharing the grant, which then refreshed with the spent token and
  // lost the whole grant (Grok's connectors, 2026-10-28).
  tx.commit();
  return t;
}

void Identity::revoke_token(std::string_view realm_name, std::string_view client_id, std::string_view token,
                            int64_t now_ms) {
  if (token.empty()) return;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt find(r.db, "SELECT client_id, user_id, family FROM tokens WHERE token_hash = ? AND kind != 'session'");
  find.bind(1, hash_secret(token));
  if (!find.step()) return;
  // RFC 7009 §2.1: a client may only revoke its own tokens; anything else is quietly ignored.
  if (!client_id.empty() && find.text(0) != client_id) return;
  int64_t user_id = find.int64(1);
  std::string family = find.is_null(2) ? "" : find.text(2);
  int n = revoke_family(r.db, family, now_ms);
  audit(r.db, now_ms, user_id, "token_revoked", user_id, bj::object{{"client_id", find.text(0)}, {"count", n}});
  tx.commit();
}

std::optional<Bearer> Identity::bearer(std::string_view realm_name, std::string_view token, int64_t now_ms) {
  if (!token.starts_with("goa_")) return std::nullopt;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db,
                 "SELECT u.id, u.email, t.client_id, t.is_agent FROM tokens t JOIN users u ON u.id = t.user_id "
                 "JOIN clients c ON c.client_id = t.client_id WHERE t.token_hash = ? AND t.kind = 'access' "
                 "AND t.revoked_at IS NULL AND t.expires_at > ? AND u.disabled_at IS NULL");
  s.bind(1, hash_secret(token)).bind(2, now_ms);
  if (!s.step()) return std::nullopt;
  Bearer b;
  b.user = User{s.int64(0), s.text(1), std::string(realm_name), {}};
  b.client_id = s.text(2);
  b.agent = s.int64(3) != 0;
  b.user.roles = roles_of(r.db, b.user.user_id);
  return b;
}

std::optional<DeviceStart> Identity::start_device(std::string_view realm_name, std::string_view client_id,
                                                  int64_t now_ms, OAuthError* error) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  auto c = find_client(r.db, client_id);
  if (!c) return *error = OAuthError{"invalid_client", "unknown client"}, std::nullopt;
  // A person client is the person's own app, with a pinned redirect; it never uses the
  // device flow, so a typed code can't mint person tokens (009 §2).
  if (c->person) {
    return *error = OAuthError{"unauthorized_client", "this client signs in with the authorization code flow"},
           std::nullopt;
  }
  Transaction tx(r.db);
  // At most 10 pending device sign-ins per client, so one can't fill the table.
  sqlite::Stmt pending(r.db,
                       "SELECT count(*) FROM device_codes WHERE client_id = ? AND expires_at > ? AND consumed_at IS NULL");
  pending.bind(1, client_id).bind(2, now_ms);
  if (pending.step() && pending.int64(0) >= 10) {
    return *error = OAuthError{"slow_down", "too many device sign-ins in progress for this client"}, std::nullopt;
  }
  DeviceStart out;
  out.device_code = base::base64url_encode(random_bytes(32));
  out.expires_in_s = kDeviceCodeTtlMs / 1000;
  out.interval_s = kDevicePollIntervalMs / 1000;
  for (int attempt = 0; attempt < 8; ++attempt) {
    out.user_code = new_user_code();
    sqlite::Stmt taken(r.db, "SELECT count(*) FROM device_codes WHERE user_code = ?");
    taken.bind(1, out.user_code);
    if (taken.step() && taken.int64(0) == 0) break;
    out.user_code.clear();
  }
  if (out.user_code.empty()) throw std::runtime_error("could not find a free device user code");
  sqlite::Stmt s(r.db,
                 "INSERT INTO device_codes (device_code_hash, user_code, client_id, expires_at) VALUES (?, ?, ?, ?)");
  s.bind(1, hash_secret(out.device_code)).bind(2, out.user_code).bind(3, client_id).bind(4, now_ms + kDeviceCodeTtlMs).run();
  tx.commit();
  return out;
}

std::optional<Client> Identity::device_client(std::string_view realm_name, std::string_view user_code_in,
                                              int64_t now_ms) {
  std::string user_code = normalize_user_code(user_code_in);
  if (user_code.empty()) return std::nullopt;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db,
                 "SELECT client_id FROM device_codes WHERE user_code = ? AND expires_at > ? AND approved_at IS NULL "
                 "AND denied_at IS NULL");
  s.bind(1, user_code).bind(2, now_ms);
  if (!s.step()) return std::nullopt;
  return find_client(r.db, s.text(0));
}

bool Identity::decide_device(std::string_view realm_name, std::string_view user_code_in, int64_t user_id,
                             bool approve, int64_t now_ms, std::string* why) {
  std::string user_code = normalize_user_code(user_code_in);
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt s(r.db,
                 approve ? "UPDATE device_codes SET approved_at = ?, user_id = ? WHERE user_code = ? AND expires_at > ? "
                           "AND approved_at IS NULL AND denied_at IS NULL"
                         : "UPDATE device_codes SET denied_at = ?, user_id = ? WHERE user_code = ? AND expires_at > ? "
                           "AND approved_at IS NULL AND denied_at IS NULL");
  s.bind(1, now_ms).bind(2, user_id).bind(3, user_code).bind(4, now_ms).run();
  if (r.db.changes() == 0) return *why = "That code isn't waiting for anyone. Check it, or start again.", false;
  audit(r.db, now_ms, user_id, approve ? "device_approved" : "device_denied", user_id,
        bj::object{{"user_code", user_code}});
  tx.commit();
  return true;
}

std::optional<Tokens> Identity::poll_device(std::string_view realm_name, std::string_view client_id,
                                            std::string_view device_code, int64_t now_ms, int64_t refresh_ms,
                                            OAuthError* error) {
  auto fail = [&](const char* code_name, const char* description) {
    *error = OAuthError{code_name, description};
    return std::nullopt;
  };
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  std::string hash = hash_secret(device_code);
  sqlite::Stmt find(r.db,
                    "SELECT client_id, user_id, expires_at, approved_at, denied_at, polled_at, consumed_at "
                    "FROM device_codes WHERE device_code_hash = ?");
  find.bind(1, hash);
  if (!find.step()) return fail("invalid_grant", "unknown device code");
  if (find.text(0) != client_id) return fail("invalid_grant", "device code issued to another client");
  if (!find.is_null(6)) return fail("invalid_grant", "device code already used");
  if (find.int64(2) <= now_ms) return fail("expired_token", "the device sign-in expired; start again");
  bool too_soon = !find.is_null(5) && now_ms - find.int64(5) < kDevicePollIntervalMs;
  bool approved = !find.is_null(3);
  bool denied = !find.is_null(4);
  int64_t user_id = find.is_null(1) ? 0 : find.int64(1);
  sqlite::Stmt polled(r.db, "UPDATE device_codes SET polled_at = ? WHERE device_code_hash = ?");
  polled.bind(1, now_ms).bind(2, hash).run();
  if (too_soon) {
    tx.commit();
    return fail("slow_down", "poll no faster than the interval");
  }
  if (denied) {
    tx.commit();
    return fail("access_denied", "the person said no");
  }
  if (!approved) {
    tx.commit();
    return fail("authorization_pending", "waiting for the person");
  }
  if (!find_client(r.db, client_id)) return fail("invalid_client", "unknown client");
  if (!user_active(r.db, user_id)) return fail("invalid_grant", "the account is disabled");
  sqlite::Stmt consume(r.db, "UPDATE device_codes SET consumed_at = ? WHERE device_code_hash = ?");
  consume.bind(1, now_ms).bind(2, hash).run();
  auto t = issue_tokens(r, client_id, user_id, true, random_hex(16), now_ms, refresh_ms);
  audit(r.db, now_ms, user_id, "token_issued", user_id,
        bj::object{{"client_id", client_id}, {"grant", "device_code"}, {"agent", true}});
  tx.commit();
  return t;
}

std::optional<std::string> Identity::mint_agent_token(std::string_view realm_name, std::string_view client_id,
                                                      std::string_view email_in, int64_t ttl_ms, int64_t now_ms,
                                                      std::string* why) {
  if (ttl_ms < 3600LL * 1000 || ttl_ms > 366LL * 24 * 3600 * 1000) return *why = "a minted token lives an hour to a year", std::nullopt;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  auto c = find_client(r.db, client_id);
  if (!c) return *why = "no client " + std::string(client_id) + "; register one with client_put, acts_as agent", std::nullopt;
  if (c->person) return *why = "only agent clients get minted tokens; a person client signs in itself", std::nullopt;
  sqlite::Stmt find(r.db, "SELECT id FROM users WHERE email = ?");
  find.bind(1, normalize_email(email_in));
  if (!find.step()) return *why = "nobody with that email has signed in to this realm yet", std::nullopt;
  int64_t user_id = find.int64(0);
  if (!user_active(r.db, user_id)) return *why = "the account is disabled", std::nullopt;
  Transaction tx(r.db);
  std::string family = random_hex(16);
  std::string token = "goa_" + random_hex(32);
  // The refresh row is never handed out; it is what lists the grant and lets the person end it.
  for (auto [kind, secret] : {std::pair<std::string_view, std::string>{"access", token}, {"refresh", "gor_" + random_hex(32)}}) {
    sqlite::Stmt ins(r.db,
                     "INSERT INTO tokens (token_hash, kind, user_id, client_id, is_agent, expires_at, family) "
                     "VALUES (?, ?, ?, ?, 1, ?, ?)");
    ins.bind(1, hash_secret(secret)).bind(2, kind).bind(3, user_id).bind(4, client_id).bind(5, now_ms + ttl_ms).bind(6, family).run();
  }
  audit(r.db, now_ms, std::nullopt, "token_minted", user_id,
        bj::object{{"client_id", client_id}, {"days", ttl_ms / (24 * 3600 * 1000)}, {"agent", true}});
  tx.commit();
  return token;
}

bj::array Identity::grants(std::string_view realm_name, int64_t user_id, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db,
                 "SELECT t.family, t.client_id, coalesce(c.name, t.client_id), coalesce(c.acts_as, 'agent'), "
                 "coalesce(c.source, ''), t.expires_at FROM tokens t LEFT JOIN clients c ON c.client_id = t.client_id "
                 "WHERE t.user_id = ? AND t.kind = 'refresh' AND t.revoked_at IS NULL AND t.rotated_to IS NULL "
                 "AND t.expires_at > ? ORDER BY t.expires_at DESC");
  s.bind(1, user_id).bind(2, now_ms);
  bj::array out;
  while (s.step()) {
    out.push_back(bj::object{{"grant", s.text(0)},
                             {"client_id", s.text(1)},
                             {"name", s.text(2)},
                             {"acts_as", s.text(3)},
                             {"source", s.text(4)},
                             {"expires_at", s.int64(5)}});
  }
  return out;
}

bool Identity::revoke_grant(std::string_view realm_name, int64_t user_id, std::string_view family, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt s(r.db, "UPDATE tokens SET revoked_at = ? WHERE family = ? AND user_id = ? AND revoked_at IS NULL");
  s.bind(1, now_ms).bind(2, family).bind(3, user_id).run();
  int n = r.db.changes();
  if (n > 0) audit(r.db, now_ms, user_id, "grant_revoked", user_id, bj::object{{"count", n}});
  tx.commit();
  return n > 0;
}

}  // namespace goober::identity
