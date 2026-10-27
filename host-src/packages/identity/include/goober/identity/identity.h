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

// OAuth 2.1 (013 § Sessions, schema, and the login API; design/machine/009.OAuth.md).
inline constexpr int64_t kAuthorizationCodeTtlMs = 60 * 1000;
inline constexpr int64_t kAccessTokenTtlMs = 60 * 60 * 1000;
inline constexpr int64_t kDeviceCodeTtlMs = 10 * 60 * 1000;
inline constexpr int64_t kDevicePollIntervalMs = 5 * 1000;

// An application or agent that may ask for tokens. A person client acts as the person,
// like their own phone; an agent client acts on their behalf, and its tokens are agent
// tokens (999 decision 1). Dynamically registered clients are always agents.
struct Client {
  std::string client_id;
  std::string name;
  std::vector<std::string> redirect_uris;
  bool person = false;
  std::string source;  // "config", "operator", "dynamic", or "host"
  int64_t created_at = 0;
};

// A redirect URI is allowed when it equals a registered one, or when both are
// http://127.0.0.1 or http://[::1] loopback URIs that differ only in the port
// (RFC 8252 §7.3; 999 decision 2). Nothing else is relaxed.
bool redirect_allowed(const Client& client, std::string_view redirect_uri);

// What a registered redirect URI may look like: https, a loopback http URI, or a
// private-use scheme with a dot in it (RFC 8252 §7.1). No fragment.
bool is_valid_redirect_uri(std::string_view uri);

struct Tokens {
  std::string access_token;
  std::string refresh_token;
  int64_t expires_in_s = 0;
};

// The person behind a bearer access token, and the client holding it.
struct Bearer {
  User user;
  std::string client_id;
  bool agent = false;
};

// A device sign-in in progress (RFC 8628).
struct DeviceStart {
  std::string device_code;
  std::string user_code;  // like WDJB-MJHT
  int64_t expires_in_s = 0;
  int64_t interval_s = 0;
};

// An OAuth error: the RFC 6749 code (invalid_grant, invalid_client, ...) and a description.
struct OAuthError {
  std::string code;
  std::string description;
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

  // ---- OAuth: clients
  // Registers or replaces a client by id (host config and operators). Refuses a bad
  // id, name, or redirect URI.
  bool put_client(std::string_view realm, const Client& client, int64_t now_ms, std::string* why);
  // Dynamic registration (RFC 7591): always an agent, with a fresh client_id.
  std::optional<Client> register_client(std::string_view realm, std::string_view name,
                                        const std::vector<std::string>& redirect_uris, int64_t now_ms,
                                        std::string* why);
  std::optional<Client> client(std::string_view realm, std::string_view client_id);
  std::vector<Client> clients(std::string_view realm);
  bool remove_client(std::string_view realm, std::string_view client_id, int64_t now_ms);

  // ---- OAuth: the authorization code flow with PKCE (S256 only)
  // Called after the person signs in and consents. Returns the raw code.
  std::string issue_code(std::string_view realm, std::string_view client_id, int64_t user_id,
                         std::string_view redirect_uri, std::string_view code_challenge, int64_t now_ms);
  // A code works once. A second use revokes the tokens the first one bought.
  std::optional<Tokens> exchange_code(std::string_view realm, std::string_view client_id, std::string_view code,
                                      std::string_view redirect_uri, std::string_view code_verifier, int64_t now_ms,
                                      int64_t refresh_ms, OAuthError* error);
  // Rotates a refresh token. Presenting one that was already rotated revokes its whole
  // grant, since someone holds a copy.
  std::optional<Tokens> refresh(std::string_view realm, std::string_view client_id, std::string_view refresh_token,
                                int64_t now_ms, int64_t refresh_ms, OAuthError* error);
  // Revokes an access or refresh token and everything else in its grant (RFC 7009).
  void revoke_token(std::string_view realm, std::string_view client_id, std::string_view token, int64_t now_ms);
  // The person behind a live access token, or nullopt.
  std::optional<Bearer> bearer(std::string_view realm, std::string_view token, int64_t now_ms);

  // ---- OAuth: the device flow, for agents without a browser
  std::optional<DeviceStart> start_device(std::string_view realm, std::string_view client_id, int64_t now_ms,
                                          OAuthError* error);
  // The client a pending user code belongs to, for the approval page.
  std::optional<Client> device_client(std::string_view realm, std::string_view user_code, int64_t now_ms);
  bool decide_device(std::string_view realm, std::string_view user_code, int64_t user_id, bool approve,
                     int64_t now_ms, std::string* why);
  std::optional<Tokens> poll_device(std::string_view realm, std::string_view client_id,
                                    std::string_view device_code, int64_t now_ms, int64_t refresh_ms,
                                    OAuthError* error);

  // A user's live grants (one per signed-in client), for the account page, and revoking one.
  bj::array grants(std::string_view realm, int64_t user_id, int64_t now_ms);
  bool revoke_grant(std::string_view realm, int64_t user_id, std::string_view family, int64_t now_ms);

  // How many users in the realm hold the role.
  int holders(std::string_view realm, std::string_view role);

  // Users with roles and live session counts, and the newest audit records.
  bj::value describe(std::string_view realm, int64_t now_ms);

 private:
  struct Realm;
  Realm& realm(std::string_view name);
  std::optional<Tokens> issue_tokens(Realm& r, std::string_view client_id, int64_t user_id, bool agent,
                                     std::string family, int64_t now_ms, int64_t refresh_ms);

  std::filesystem::path directory_;
  Mailer mailer_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<Realm>, std::less<>> realms_;
};

}  // namespace goober::identity
