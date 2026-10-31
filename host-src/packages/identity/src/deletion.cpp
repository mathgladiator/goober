// Deleting an account (design/machine/021.IdentityEvents.md § 6): erased, with a tombstone that
// keeps the id, and a "deleted" event boxes hear so they erase their rows too.

#include <variant>

#include "goober/identity/identity.h"
#include "internal.h"

namespace goober::identity {

using namespace detail;

std::optional<Deletion> Identity::delete_user(std::string_view realm_name, int64_t user_id, std::string_view how,
                                              std::string_view by, int64_t now_ms, std::string* why) {
  if (realm_name == kHostRealm) return *why = "host operators come from identity.operators in the host config", std::nullopt;
  if (how != "self" && how != "operator") return *why = "how is self or operator", std::nullopt;
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  std::string email;
  {
    sqlite::Stmt s(r.db, "SELECT email, deleted_at FROM users WHERE id = ?");
    s.bind(1, user_id);
    if (!s.step()) return *why = "no user " + std::to_string(user_id) + " in " + std::string(realm_name), std::nullopt;
    if (!s.is_null(1)) return *why = "user " + std::to_string(user_id) + " is already deleted", std::nullopt;
    email = s.text(0);
  }
  std::string tombstone = "deleted:" + std::to_string(user_id);  // never an address: no @
  auto run = [&](const char* sql, std::initializer_list<std::variant<int64_t, std::string>> binds) {
    sqlite::Stmt s(r.db, sql);
    int i = 1;
    for (const auto& b : binds) {
      if (std::holds_alternative<int64_t>(b)) s.bind(i++, std::get<int64_t>(b));
      else s.bind(i++, std::string_view(std::get<std::string>(b)));
    }
    s.run();
  };
  // Everything that lets anyone act as them ends.
  run("UPDATE tokens SET revoked_at = ? WHERE user_id = ? AND revoked_at IS NULL", {now_ms, user_id});
  run("UPDATE authorization_codes SET consumed_at = ? WHERE user_id = ? AND consumed_at IS NULL", {now_ms, user_id});
  run("UPDATE device_codes SET denied_at = ? WHERE user_id = ? AND approved_at IS NULL AND denied_at IS NULL",
      {now_ms, user_id});
  run("DELETE FROM email_codes WHERE email = ?", {email});
  run("DELETE FROM roles WHERE user_id = ?", {user_id});
  run("UPDATE roles SET granted_by = NULL WHERE granted_by = ?", {user_id});
  // The address leaves the realm: the account, its past events, its audit rows, and invites to it
  // (an open one is revoked: nobody is left to take it up).
  run("UPDATE users SET email = ?, deleted_at = ?, disabled_at = coalesce(disabled_at, ?) WHERE id = ?",
      {tombstone, now_ms, now_ms, user_id});
  run("UPDATE events SET email = '' WHERE user_id = ?", {user_id});
  run("UPDATE invites SET state = 'revoked', answered_at = ?, answer_how = 'account deleted' WHERE email = ? AND "
      "state = 'open'",
      {now_ms, email});
  run("UPDATE invites SET email = ? WHERE email = ?", {tombstone, email});
  run("UPDATE invites SET inviter_email = ? WHERE inviter_email = ?", {tombstone, email});
  {
    // Audit rows about them keep what happened, not the address.
    std::vector<std::pair<int64_t, std::string>> rows;
    sqlite::Stmt s(r.db, "SELECT id, detail FROM audit WHERE subject = ? OR actor = ?");
    s.bind(1, user_id).bind(2, user_id);
    while (s.step()) rows.emplace_back(s.int64(0), s.text(1));
    for (auto& [id, text] : rows) {
      std::string err;
      auto v = json::parse(text, &err);
      if (!v || !v->is_object()) continue;
      bool changed = false;
      for (auto& kv : v->as_object()) {
        if (kv.value().is_string() && kv.value().as_string() == email) {
          kv.value() = tombstone;
          changed = true;
        }
      }
      if (changed) run("UPDATE audit SET detail = ? WHERE id = ?", {json::dump(*v), id});
    }
  }
  run("INSERT INTO events (kind, user_id, email, at) VALUES (?, ?, '', ?)", {std::string(kDeleted), user_id, now_ms});
  int64_t event_id = r.db.last_insert_rowid();
  run("INSERT INTO deletions (event_id, user_id, at, how) VALUES (?, ?, ?, ?)",
      {event_id, user_id, now_ms, std::string(how)});
  audit(r.db, now_ms, how == "self" ? std::optional<int64_t>(user_id) : std::nullopt, "user_deleted", user_id,
        bj::object{{"how", how}, {"by", by}, {"event_id", std::to_string(event_id)}});
  tx.commit();
  return Deletion{event_id, user_id, now_ms, std::string(how)};
}

std::optional<User> Identity::find_user(std::string_view realm_name, std::string_view email_in) {
  std::string email = normalize_email(email_in);
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "SELECT id FROM users WHERE email = ? AND deleted_at IS NULL");
  s.bind(1, email);
  if (!s.step()) return std::nullopt;
  int64_t id = s.int64(0);
  return User{id, email, std::string(realm_name), roles_of(r.db, id)};
}

std::optional<User> Identity::active_user(std::string_view realm_name, std::string_view email_in) {
  std::string email = normalize_email(email_in);
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "SELECT id FROM users WHERE email = ? AND deleted_at IS NULL AND disabled_at IS NULL");
  s.bind(1, email);
  if (!s.step()) return std::nullopt;
  int64_t id = s.int64(0);
  return User{id, email, std::string(realm_name), roles_of(r.db, id)};
}

std::vector<Deletion> Identity::deletions(std::string_view realm_name, int limit) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "SELECT event_id, user_id, at, how FROM deletions ORDER BY event_id DESC LIMIT ?");
  s.bind(1, int64_t(limit));
  std::vector<Deletion> out;
  while (s.step()) out.push_back(Deletion{s.int64(0), s.int64(1), s.int64(2), s.text(3)});
  return out;
}

}  // namespace goober::identity
