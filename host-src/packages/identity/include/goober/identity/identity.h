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
// Wrong codes allowed per address per day, across all its codes.
inline constexpr int kFailuresPerDay = 20;
inline constexpr int64_t kFailureWindowMs = 24LL * 3600 * 1000;
// An account whose address was never proved by a code (an invite, made by a grant) is
// deleted, with its roles, this long after it was made.
inline constexpr int64_t kUnverifiedTtlMs = 30LL * 24 * 3600 * 1000;
// Identity events are kept this long for boxes' identity routes to read.
inline constexpr int64_t kEventTtlMs = 7LL * 24 * 3600 * 1000;

// What boxes' identity routes hear (design/machine/021.IdentityEvents.md).
inline constexpr std::string_view kFirstVerified = "first-verified";  // an address proved for the first time
inline constexpr std::string_view kSignedIn = "signed-in";            // every sign-in by an emailed code
inline constexpr std::string_view kInviteAccepted = "invite-accepted";  // design/machine/029.Invites.md § 2.5
inline constexpr std::string_view kInviteDeclined = "invite-declined";
inline constexpr std::string_view kDeleted = "deleted";  // an account erased (021 § 6); its event holds no address

// A deleted account, kept for good without its address, so boxes can be held to erasing their
// rows (021 § 6).
struct Deletion {
  int64_t event_id = 0;  // the "deleted" event's id in the realm
  int64_t user_id = 0;
  int64_t at_ms = 0;
  std::string how;  // "self" (the person, on their account page) or "operator"
};

// Invites (029 § 2): one reminder after 7 days, lapsed at 30, and the address cools 90 days.
inline constexpr int64_t kInviteTtlMs = 30LL * 24 * 3600 * 1000;
inline constexpr int64_t kInviteReminderMs = 7LL * 24 * 3600 * 1000;
inline constexpr int64_t kInviteCoolMs = 90LL * 24 * 3600 * 1000;

struct Mail {
  std::string to;
  std::string subject;
  std::string text;
  std::string from_name = {};  // the sender's display name; "" means goober
  std::string reply_to = {};   // an address replies go to, or ""
  // A host mail rendered from a template (design/machine/022.MailTemplates.md): the realm whose
  // templates apply, the template's name, and its values. Without a name, subject and text go as they are.
  std::string realm = {};
  std::string template_name = {};
  bj::object vars = {};
  std::string html = {};  // an HTML alternative to text, or ""
  // A box's mail (design/machine/018.Mail.md § 3): its own From on the tenant's domain, the
  // headers that thread it into a conversation, and the one-click unsubscribe (RFC 8058).
  std::string from_address = {};    // "" means the mailer's
  std::string in_reply_to = {};     // "<id@host>", or ""
  std::string references = {};      // "<a@x> <b@y>", or ""
  std::string list_unsubscribe = {};  // "<https://...>", or ""
  std::string list_id = {};           // "<digest.kc.example.com>" for a list's mail (RFC 2919), or ""
  std::string tenant = {};            // whose box sent it, for logs/mail.log; "" for host mail
  bool test = false;                  // an operator's mail_template_send_test, marked so in logs/mail.log
  // A calendar invite (RFC 5546): its text/calendar body, sent beside the text and HTML and as
  // invite.ics, with its METHOD (REQUEST or CANCEL).
  std::string calendar = {};
  std::string calendar_method = {};
  // Files attached (a box's attachments, design/machine/783.ThesisCommunity.md § 3).
  struct File {
    std::string filename, content_type, bytes;
  };
  std::vector<File> attachments = {};
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
  bool created = false;         // the user was created by this sign-in
  bool first_verified = false;  // the address was proved for the first time by this sign-in
};

// One identity event, appended in the same transaction as the sign-in that caused it.
struct Event {
  int64_t id = 0;  // increasing within the realm, never reused
  std::string kind;  // kFirstVerified, kSignedIn, kInviteAccepted, kInviteDeclined, or kDeleted
  int64_t user_id = 0;  // 0 for invite-declined: nobody signed in
  std::string email;
  int64_t at_ms = 0;
  int64_t invite_id = 0;  // for the invite events
  std::string box;        // the box that sent the invite, or ""
};

// One invite (029 § 2.7).
struct Invite {
  int64_t id = 0;
  std::string email;
  std::string state = "open";  // open, accepted, declined, lapsed, revoked
  std::vector<std::string> roles, lists;
  std::string note, inviter_name, inviter_email;
  std::string inviter_kind;  // operator, person, site
  std::string box;           // "" for an operator
  std::string return_path = "/";
  int64_t created_at = 0, reminded_at = 0, answered_at = 0, expires_at = 0;
  std::string answer_how;
  std::string terms_sha256;
  bj::object to_json() const;
};

// OAuth 2.1 (013 § Sessions, schema, and the login API; design/machine/009.OAuth.md).
inline constexpr int64_t kAuthorizationCodeTtlMs = 60 * 1000;
inline constexpr int64_t kAccessTokenTtlMs = 60 * 60 * 1000;
// A spent refresh token used again this soon after it was rotated is a client racing itself
// (two sessions, or a retry after a lost reply), not a thief: it gets tokens, not a revoked grant.
inline constexpr int64_t kRefreshReuseGraceMs = 60 * 1000;
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
  // Where new users' and invites' ids come from: the host's id service (038 § 2). Without
  // one, each realm's AUTOINCREMENT numbers them. Set before the first call.
  void set_id_source(std::function<int64_t()> ids) { ids_ = std::move(ids); }
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
  // A user by id, with their roles; nullopt when gone or disabled.
  std::optional<User> user(std::string_view realm, int64_t user_id);
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

  // An operator mints an agent's access token directly, for agents that send one fixed
  // bearer header and can't refresh (Grok's remote MCP, scripts). The client must act as an
  // agent and the person must already exist. It lives `ttl_ms` (an hour to a year), shows on
  // the person's account page as a grant, and the person can revoke it there.
  std::optional<std::string> mint_agent_token(std::string_view realm, std::string_view client_id,
                                              std::string_view email, int64_t ttl_ms, int64_t now_ms,
                                              std::string* why);

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

  // Deletes expired codes, week-old dynamic clients that never signed anyone in, events past
  // kEventTtlMs, and accounts never proved within kUnverifiedTtlMs (with their roles).
  // Returns how many accounts were deleted.
  int prune(std::string_view realm, int64_t now_ms);

  // ---- deleting an account (design/machine/021.IdentityEvents.md § 6)
  // Erases a user: ends every session, token, and grant; drops roles, pending codes, and open
  // invites to the address; replaces the address everywhere the realm keeps it (the account,
  // its events, its audit rows, its invites) with "deleted:$id". The id stays, never reused and
  // never signed in again; the same address may sign up again as a new user. Appends a
  // "deleted" event (no address) and a lasting deletions row. `how` is "self" or "operator";
  // `by` names who, for the audit. Refused in the host realm (operators are config).
  std::optional<Deletion> delete_user(std::string_view realm, int64_t user_id, std::string_view how,
                                      std::string_view by, int64_t now_ms, std::string* why);
  // The live (not deleted) user with this address, with their roles.
  std::optional<User> find_user(std::string_view realm, std::string_view email);
  // The user an address belongs to when the account can act: neither deleted nor disabled. The
  // address is matched whole, lowercased; a plus-tag is part of it (024 § 5).
  std::optional<User> active_user(std::string_view realm, std::string_view email);
  // Every deletion in the realm, newest first, at most `limit`.
  std::vector<Deletion> deletions(std::string_view realm, int limit);

  // Identity events after `after_id`, oldest first, at most `limit`; and the newest event's id
  // (0 when there is none).
  std::vector<Event> events_after(std::string_view realm, int64_t after_id, int limit);
  int64_t last_event_id(std::string_view realm);

  // A user's browser sessions, for the account page: {id, expires_at}, and ending one.
  bj::array sessions(std::string_view realm, int64_t user_id, int64_t now_ms);
  bool end_session_by_id(std::string_view realm, int64_t user_id, int64_t id, int64_t now_ms);

  // A user's live grants (one per signed-in client), for the account page, and revoking one.
  bj::array grants(std::string_view realm, int64_t user_id, int64_t now_ms);
  bool revoke_grant(std::string_view realm, int64_t user_id, std::string_view family, int64_t now_ms);

  // ---- invites (design/machine/029.Invites.md § 2): the record; the host checks blocks, config, and limits.
  // Stores an open invite. Refuses, with `code` (member, open, cooling) and `why`, an address
  // that is already a proved account, has an open invite, or lapsed or was revoked within 90 days.
  std::optional<Invite> create_invite(std::string_view realm, Invite invite, int64_t now_ms, std::string* code,
                                      std::string* why);
  std::optional<Invite> invite(std::string_view realm, int64_t id);
  std::optional<Invite> open_invite_for(std::string_view realm, std::string_view email);
  // Newest first; empty filters match everything.
  std::vector<Invite> invites(std::string_view realm, std::string_view state, std::string_view email,
                              std::string_view box, int limit);
  // Invites made since `since_ms`, all or by one inviter's address.
  int64_t invites_since(std::string_view realm, int64_t since_ms, std::string_view inviter_email = {});
  bool revoke_invite(std::string_view realm, int64_t id, std::string_view by, int64_t now_ms, std::string* why);
  // Accepts an open invite for the signed-in user whose address it names: grants its roles (each
  // checked by `allowed`), appends invite-accepted, and returns the user. The user must hold the
  // invited address and have proved it.
  std::optional<User> accept_invite(std::string_view realm, int64_t id, int64_t user_id, std::string_view how,
                                    const std::function<bool(const std::string& role)>& allowed, int64_t now_ms,
                                    std::string* why);
  // Declines an open or lapsed invite, appending invite-declined. False when it was already answered.
  bool decline_invite(std::string_view realm, int64_t id, std::string_view how, int64_t now_ms, std::string* why);
  // Open invites due their one reminder (7 days old, none sent), and marking one sent.
  std::vector<Invite> due_reminders(std::string_view realm, int64_t now_ms);
  void mark_reminded(std::string_view realm, int64_t id, int64_t now_ms);

  // Whether the address is an account of the realm that has proved it (by a code).
  bool proved(std::string_view realm, std::string_view email);

  // ---- terms (029 § 8): the newest hash of a terms page a user accepted, and recording one.
  std::optional<std::string> terms_accepted(std::string_view realm, int64_t user_id, std::string_view path);
  void accept_terms(std::string_view realm, int64_t user_id, std::string_view path, std::string_view sha256,
                    std::string_view how, int64_t now_ms);

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
  std::function<int64_t()> ids_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<Realm>, std::less<>> realms_;
};

}  // namespace goober::identity
