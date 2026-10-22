#pragma once

// The built-in identity service (013.Identity.md), v1 subset: passwordless sign-in
// by an emailed one-time code, opaque session tokens, grant roles, and an audit.
// One SQLite database per realm, under the identity directory: a tenant's realm is
// $directory/$tenant.db, and host identities live in $directory/_host.db.
// See design/machine/006.LocalIdentity.md.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"

namespace goober::identity {

namespace bj = boost::json;

// The platform's realm, which holds host identities.
inline constexpr std::string_view kHostRealm = "_host";

inline constexpr int64_t kCodeTtlMs = 10 * 60 * 1000;
inline constexpr int kCodeMaxAttempts = 5;
// Codes mailed to one address within one code lifetime, after which starts are quietly dropped.
inline constexpr int kCodesPerWindow = 5;

struct Mail {
  std::string to;
  std::string subject;
  std::string text;
};
using Mailer = std::function<void(const Mail&)>;

struct User {
  int64_t user_id = 0;
  std::string email;
  std::string realm;               // a tenant, or _host
  std::vector<std::string> roles;  // the grant roles stored in the realm, sorted
};

struct SignIn {
  std::string token;  // the raw session token; only its hash is stored
  User user;
  bool created = false;  // the user was created by this sign-in
};

// A tenant role: "role" or "role/scope", each part [A-Za-z0-9_]+. No ":", which
// only host roles carry.
bool is_valid_tenant_role(std::string_view role);

// A realm name: a valid tenant name, or _host.
bool is_valid_realm(std::string_view realm);

class Identity {
 public:
  Identity(std::filesystem::path directory, Mailer mailer);
  ~Identity();
  Identity(const Identity&) = delete;
  Identity& operator=(const Identity&) = delete;

  // Mails a one-time code to `email` for signing in to `realm`, naming `where` (the
  // domain) in the mail. With `signup` false, only an existing user gets a code.
  // The caller learns nothing either way, so the screen can't reveal who has an account.
  void start_email(std::string_view realm, std::string_view email, std::string_view where, bool signup,
                   int64_t now_ms);

  // Checks a code. On success, creates the user when `signup` allows and the
  // address is new, and opens a session lasting `session_ms`.
  std::optional<SignIn> verify_email(std::string_view realm, std::string_view email, std::string_view code,
                                     bool signup, int64_t now_ms, int64_t session_ms, std::string* why);

  // The user behind a live session token, or nullopt.
  std::optional<User> session(std::string_view realm, std::string_view token, int64_t now_ms);
  void end_session(std::string_view realm, std::string_view token, int64_t now_ms);

  // Operator actions, audited with actor null and detail {"by": by}.
  // grant creates the user when the address is new, so a role can wait for someone.
  bool grant(std::string_view realm, std::string_view email, std::string_view role, std::string_view by,
             int64_t now_ms, std::string* why);
  bool revoke(std::string_view realm, std::string_view email, std::string_view role, std::string_view by,
              int64_t now_ms, std::string* why);
  // Ends every session of a user. Returns how many ended.
  int revoke_sessions(std::string_view realm, std::string_view email, std::string_view by, int64_t now_ms);

  // How many users in the realm hold the role.
  int holders(std::string_view realm, std::string_view role);

  // Users with roles and live session counts, and the newest audit records.
  bj::value describe(std::string_view realm, int64_t now_ms);

 private:
  struct Realm;
  Realm& realm(std::string_view name);

  std::filesystem::path directory_;
  Mailer mailer_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<Realm>, std::less<>> realms_;
};

}  // namespace goober::identity
