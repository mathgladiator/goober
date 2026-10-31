// Invites and terms (design/machine/029.Invites.md §§ 2, 8): the realm's record of who was
// invited, what accepting grants, and how each invite ended. The host checks blocks, the host
// config, and the daily limits before it calls create_invite; this keeps the record's own rules.

#include "internal.h"

namespace goober::identity {

using namespace detail;

namespace {

bj::array to_array(const std::vector<std::string>& v) {
  bj::array a;
  for (const auto& s : v) a.push_back(bj::value(s));
  return a;
}

std::vector<std::string> from_json_array(const std::string& text) {
  std::string err;
  auto v = json::parse(text, &err);
  std::vector<std::string> out;
  if (v && v->is_array()) {
    for (const auto& e : v->as_array()) {
      if (e.is_string()) out.push_back(std::string(e.as_string()));
    }
  }
  return out;
}

constexpr const char* kColumns =
    "id, email, state, roles, lists, note, inviter_name, inviter_email, inviter_kind, box, return_path, created_at, "
    "reminded_at, answered_at, expires_at, answer_how, terms_sha256";

Invite read_invite(sqlite::Stmt& s) {
  Invite i;
  i.id = s.int64(0);
  i.email = s.text(1);
  i.state = s.text(2);
  i.roles = from_json_array(s.text(3));
  i.lists = from_json_array(s.text(4));
  i.note = s.text(5);
  i.inviter_name = s.text(6);
  i.inviter_email = s.text(7);
  i.inviter_kind = s.text(8);
  i.box = s.is_null(9) ? "" : s.text(9);
  i.return_path = s.text(10);
  i.created_at = s.int64(11);
  i.reminded_at = s.is_null(12) ? 0 : s.int64(12);
  i.answered_at = s.is_null(13) ? 0 : s.int64(13);
  i.expires_at = s.int64(14);
  i.answer_how = s.is_null(15) ? "" : s.text(15);
  i.terms_sha256 = s.text(16);
  return i;
}

std::optional<Invite> invite_by_id(sqlite::Db& db, int64_t id) {
  sqlite::Stmt s(db, std::string("SELECT ") + kColumns + " FROM invites WHERE id = ?");
  s.bind(1, id);
  if (!s.step()) return std::nullopt;
  return read_invite(s);
}

void append_event(sqlite::Db& db, std::string_view kind, int64_t user_id, std::string_view email, int64_t now_ms,
                  int64_t invite_id, const std::string& box) {
  sqlite::Stmt e(db, "INSERT INTO events (kind, user_id, email, at, invite_id, box) VALUES (?, ?, ?, ?, ?, ?)");
  e.bind(1, kind).bind(2, user_id).bind(3, email).bind(4, now_ms).bind(5, invite_id);
  box.empty() ? e.bind_null(6) : e.bind(6, box);
  e.run();
}

}  // namespace

bj::object Invite::to_json() const {
  bj::object o{{"invite_id", std::to_string(id)},
               {"email", email},
               {"state", state},
               {"roles", to_array(roles)},
               {"lists", to_array(lists)},
               {"inviter", bj::object{{"kind", inviter_kind}, {"name", inviter_name}, {"email", inviter_email}}},
               {"box", box.empty() ? bj::value(nullptr) : bj::value(box)},
               {"return", return_path},
               {"created_at", created_at},
               {"expires_at", expires_at}};
  if (!note.empty()) o["note"] = note;
  if (reminded_at) o["reminded_at"] = reminded_at;
  if (answered_at) o["answered_at"] = answered_at;
  if (!answer_how.empty()) o["answer_how"] = answer_how;
  return o;
}

std::optional<Invite> Identity::create_invite(std::string_view realm_name, Invite in, int64_t now_ms, std::string* code,
                                              std::string* why) {
  in.email = normalize_email(in.email);
  if (!looks_like_email(in.email)) return *code = "bad_input", *why = "not an email address", std::nullopt;
  if (realm_name == kHostRealm) return *code = "bad_input", *why = "host identities aren't invited", std::nullopt;
  for (const auto& role : in.roles) {
    if (!is_valid_tenant_role(role)) return *code = "bad_input", *why = "not a tenant role: " + role, std::nullopt;
  }
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt member(r.db, "SELECT 1 FROM users WHERE email = ? AND verified_at IS NOT NULL");
  member.bind(1, in.email);
  if (member.step()) {
    return *code = "member", *why = in.email + " already has an account here; grant it a role instead", std::nullopt;
  }
  sqlite::Stmt open(r.db, "SELECT id FROM invites WHERE email = ? AND state = 'open'");
  open.bind(1, in.email);
  if (open.step()) {
    return *code = "open", *why = "invite " + std::to_string(open.int64(0)) + " to " + in.email + " is still open",
           std::nullopt;
  }
  sqlite::Stmt cool(r.db,
                    "SELECT max(answered_at) FROM invites WHERE email = ? AND state IN ('lapsed', 'revoked') AND "
                    "answered_at > ?");
  cool.bind(1, in.email).bind(2, now_ms - kInviteCoolMs);
  if (cool.step() && !cool.is_null(0)) {
    int64_t until = cool.int64(0) + kInviteCoolMs;
    return *code = "cooling", *why = in.email + " can be invited again after " + std::to_string(until) + " (ms)",
           std::nullopt;
  }
  in.state = "open";
  in.created_at = now_ms;
  in.expires_at = now_ms + kInviteTtlMs;
  in.reminded_at = in.answered_at = 0;
  sqlite::Stmt s(r.db,
                 "INSERT INTO invites (email, state, roles, lists, note, inviter_name, inviter_email, inviter_kind, box, "
                 "return_path, created_at, expires_at, terms_sha256, id) VALUES (?, 'open', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  if (ids_) s.bind(13, ids_());
  else s.bind_null(13);  // AUTOINCREMENT's next
  s.bind(1, in.email).bind(2, json::dump(to_array(in.roles))).bind(3, json::dump(to_array(in.lists))).bind(4, in.note);
  s.bind(5, in.inviter_name).bind(6, in.inviter_email).bind(7, in.inviter_kind);
  in.box.empty() ? s.bind_null(8) : s.bind(8, in.box);
  s.bind(9, in.return_path).bind(10, now_ms).bind(11, in.expires_at).bind(12, in.terms_sha256).run();
  in.id = r.db.last_insert_rowid();
  audit(r.db, now_ms, std::nullopt, "invite_created", std::nullopt,
        bj::object{{"invite_id", in.id}, {"email", in.email}, {"roles", to_array(in.roles)},
                   {"by", in.box.empty() ? "operator" : "box:" + in.box}});
  tx.commit();
  return in;
}

std::optional<Invite> Identity::invite(std::string_view realm_name, int64_t id) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  return invite_by_id(r.db, id);
}

std::optional<Invite> Identity::open_invite_for(std::string_view realm_name, std::string_view email) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, std::string("SELECT ") + kColumns + " FROM invites WHERE email = ? AND state = 'open' AND expires_at > ? "
                                                           "ORDER BY id DESC LIMIT 1");
  s.bind(1, normalize_email(email)).bind(2, int64_t(0));
  if (!s.step()) return std::nullopt;
  return read_invite(s);
}

std::vector<Invite> Identity::invites(std::string_view realm_name, std::string_view state, std::string_view email,
                                      std::string_view box, int limit) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, std::string("SELECT ") + kColumns +
                           " FROM invites WHERE (? = '' OR state = ?) AND (? = '' OR email = ?) AND (? = '' OR box = ?) "
                           "ORDER BY id DESC LIMIT ?");
  std::string e = normalize_email(email), st(state), bx(box);
  s.bind(1, st).bind(2, st).bind(3, e).bind(4, e).bind(5, bx).bind(6, bx).bind(7, int64_t(limit));
  std::vector<Invite> out;
  while (s.step()) out.push_back(read_invite(s));
  return out;
}

int64_t Identity::invites_since(std::string_view realm_name, int64_t since_ms, std::string_view inviter_email) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "SELECT count(*) FROM invites WHERE created_at > ? AND (? = '' OR inviter_email = ?)");
  std::string by(inviter_email);  // an empty view may have no data, which would bind NULL
  s.bind(1, since_ms).bind(2, by).bind(3, by);
  return s.step() ? s.int64(0) : 0;
}

bool Identity::revoke_invite(std::string_view realm_name, int64_t id, std::string_view by, int64_t now_ms,
                             std::string* why) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  sqlite::Stmt s(r.db, "UPDATE invites SET state = 'revoked', answered_at = ?, answer_how = ? WHERE id = ? AND state = 'open'");
  s.bind(1, now_ms).bind(2, by).bind(3, id).run();
  if (r.db.changes() == 0) return *why = "invite " + std::to_string(id) + " isn't open", false;
  audit(r.db, now_ms, std::nullopt, "invite_revoked", std::nullopt, bj::object{{"invite_id", id}, {"by", by}});
  tx.commit();
  return true;
}

std::optional<User> Identity::accept_invite(std::string_view realm_name, int64_t id, int64_t user_id,
                                            std::string_view how,
                                            const std::function<bool(const std::string& role)>& allowed,
                                            int64_t now_ms, std::string* why) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  auto inv = invite_by_id(r.db, id);
  if (!inv || inv->state != "open" || inv->expires_at <= now_ms) return *why = "this invite isn't open", std::nullopt;
  sqlite::Stmt u(r.db, "SELECT email, verified_at FROM users WHERE id = ? AND disabled_at IS NULL");
  u.bind(1, user_id);
  if (!u.step() || u.text(0) != inv->email) return *why = "this invite is for another address", std::nullopt;
  if (u.is_null(1)) return *why = "the address isn't proved yet", std::nullopt;
  std::vector<std::string> granted;
  for (const auto& role : inv->roles) {
    if (allowed && !allowed(role)) continue;  // the host config dropped it since the invite went
    sqlite::Stmt g(r.db, "INSERT OR IGNORE INTO roles (user_id, role, granted_by, granted_at) VALUES (?, ?, NULL, ?)");
    g.bind(1, user_id).bind(2, role).bind(3, now_ms).run();
    if (r.db.changes() > 0) {
      audit(r.db, now_ms, std::nullopt, "grant", user_id, bj::object{{"role", role}, {"by", "invite:" + std::to_string(id)}});
    }
    granted.push_back(role);
  }
  sqlite::Stmt s(r.db, "UPDATE invites SET state = 'accepted', answered_at = ?, answer_how = ? WHERE id = ?");
  s.bind(1, now_ms).bind(2, how).bind(3, id).run();
  append_event(r.db, kInviteAccepted, user_id, inv->email, now_ms, id, inv->box);
  audit(r.db, now_ms, user_id, "invite_accepted", user_id, bj::object{{"invite_id", id}, {"roles", to_array(granted)}});
  User out{user_id, inv->email, std::string(realm_name), roles_of(r.db, user_id)};
  tx.commit();
  return out;
}

bool Identity::decline_invite(std::string_view realm_name, int64_t id, std::string_view how, int64_t now_ms,
                              std::string* why) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  Transaction tx(r.db);
  auto inv = invite_by_id(r.db, id);
  if (!inv) return *why = "no such invite", false;
  // Saying no is always possible: a lapsed invite's decline link still declines (029 § 2.4).
  if (inv->state != "open" && inv->state != "lapsed") return *why = "this invite was already " + inv->state, false;
  sqlite::Stmt s(r.db, "UPDATE invites SET state = 'declined', answered_at = ?, answer_how = ? WHERE id = ?");
  s.bind(1, now_ms).bind(2, how).bind(3, id).run();
  append_event(r.db, kInviteDeclined, 0, inv->email, now_ms, id, inv->box);
  audit(r.db, now_ms, std::nullopt, "invite_declined", std::nullopt, bj::object{{"invite_id", id}, {"how", how}});
  tx.commit();
  return true;
}

std::vector<Invite> Identity::due_reminders(std::string_view realm_name, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, std::string("SELECT ") + kColumns +
                           " FROM invites WHERE state = 'open' AND reminded_at IS NULL AND created_at <= ? AND "
                           "expires_at > ? ORDER BY id LIMIT 100");
  s.bind(1, now_ms - kInviteReminderMs).bind(2, now_ms);
  std::vector<Invite> out;
  while (s.step()) out.push_back(read_invite(s));
  return out;
}

void Identity::mark_reminded(std::string_view realm_name, int64_t id, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "UPDATE invites SET reminded_at = ? WHERE id = ? AND reminded_at IS NULL");
  s.bind(1, now_ms).bind(2, id).run();
}

bool Identity::proved(std::string_view realm_name, std::string_view email) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "SELECT 1 FROM users WHERE email = ? AND verified_at IS NOT NULL AND disabled_at IS NULL");
  s.bind(1, normalize_email(email));
  return s.step();
}

std::optional<std::string> Identity::terms_accepted(std::string_view realm_name, int64_t user_id, std::string_view path) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "SELECT sha256 FROM terms_accepted WHERE user_id = ? AND path = ? ORDER BY at DESC LIMIT 1");
  s.bind(1, user_id).bind(2, path);
  if (!s.step()) return std::nullopt;
  return s.text(0);
}

void Identity::accept_terms(std::string_view realm_name, int64_t user_id, std::string_view path, std::string_view sha256,
                            std::string_view how, int64_t now_ms) {
  auto& r = realm(realm_name);
  std::lock_guard lock(r.mu);
  sqlite::Stmt s(r.db, "INSERT OR IGNORE INTO terms_accepted (user_id, path, sha256, at, how) VALUES (?, ?, ?, ?, ?)");
  s.bind(1, user_id).bind(2, path).bind(3, sha256).bind(4, now_ms).bind(5, how).run();
}

}  // namespace goober::identity
