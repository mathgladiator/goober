#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>

#include "goober/attachments/attachments.h"
#include "goober/bus/bus.h"
#include "goober/boxes/registry.h"
#include "goober/config/config.h"
#include "goober/content/content.h"
#include "goober/db/database.h"
#include "goober/identity/identity.h"
#include "goober/logs/logs.h"
#include "goober/mail_consent/consent.h"
#include "goober/mail_inbound/mime.h"
#include "goober/mail_templates/templates.h"
#include "goober/host/rate_limit.h"
#include "goober/host/worker_pool.h"
#include "goober/mcp/mcp.h"
#include "goober/operations/changes.h"
#include "goober/operations/live.h"
#include "goober/operations/traffic.h"
#include "goober/runtime_wasm/engine.h"
#include "goober/server_http/server.h"
#include "goober/site/site.h"
#include "goober/source/source.h"
#include "goober/specs/specs.h"
#include "goober/vault/vault.h"
#include "goober/wal/wal.h"
#include "goober/webpush/webpush.h"

namespace goober::host {

inline constexpr std::string_view kVersion = "0.1.0";
inline constexpr std::string_view kOperationsMcpPath = "/goober-operations-mcp";
// Where Stalwart calls the host at each SMTP stage (038; 041 § /internal/).
inline constexpr std::string_view kMailHookPath = "/internal/routes/mta/stalwart";
// How large an operator's request may be: a pushed site file or a box, as base64 in JSON.
inline constexpr uint64_t kOperatorBodyMaxBytes = 64ull << 20;
inline constexpr uint64_t kOperatorAttachmentMaxBytes = 32ull << 20;
// An image an editor uploads from the attachments page, signed in with a browser session.
inline constexpr uint64_t kBrowserAttachmentMaxBytes = 1ull << 20;
// Room for a multipart form's boundaries and fields around the file.
inline constexpr uint64_t kMultipartOverheadBytes = 64ull << 10;

// A signed-in caller: a tenant identity in the domain's realm, or a host identity
// whose roles come from identity.operators in the config.
struct SignedIn {
  identity::User user;
  bool host = false;
  std::vector<std::string> roles;
  // A bearer token from an agent client (050): only routes with "agents": true accept it.
  bool agent = false;
  std::string client_id;  // the OAuth client behind a bearer token, or ""
};

// The whole host: config, storage, the WAL, content, boxes, the engine, the
// operator MCP endpoint, and the HTTP server.
// Answers one request: the response, plus fields for its access-log line (a JSON object, or "").
using Finish = std::function<void(server::http::Response, std::string)>;

class Host {
 public:
  // Loads the config file (or takes a parsed config, for tests) and recovers storage.
  explicit Host(std::shared_ptr<const config::HostConfig> config, std::filesystem::path config_file = {});
  ~Host();

  // Starts serving. Returns the bound port.
  uint16_t start();
  void stop();

  // Rereads the config file. Returns a note for the log.
  std::string reload();

  // Timers (045; design/machine/014.Time.md): runs every fire due at now_ms. A host thread
  // calls it each second; tests call it with their own clock, after set_timers_inline(true)
  // so fires run on the calling thread.
  void timer_tick(int64_t now_ms);
  void set_timers_inline(bool inline_fires);
  // For tests: one goober_call as a box would make it, for a tenant, with nobody signed in; an
  // outbox call's element is enqueued at once (as a request that committed). Returns the answer.
  std::string call_as_box(const std::string& box, const std::string& tenant, const std::string& service,
                          const std::string& method, const std::string& request, const std::string& user_id = "");
  // What the scheduler plans next, and firing a timer now for one tenant (operators):
  // returns how many fires ran, or nullopt with `error`.
  boost::json::array timers_due();
  std::optional<int> run_timer_now(const std::string& name, const std::string& tenant, std::string* error);
  // Identity routes (design/machine/021.IdentityEvents.md): delivers sign-in events to the boxes
  // that declare them, and hourly prunes the realms. The timer thread calls it each second.
  void identity_tick(int64_t now_ms);

  // The outbox (035; design/machine/015.Network.md): delivers what's due and returns how
  // many were delivered. A host thread calls it every two seconds; tests call it directly.
  int outbox_tick(int64_t now_ms);
  boost::json::array outbox_list(const std::string& state);
  // Retries (or, with retry false, discards) a dead element.
  bool outbox_set(const std::string& tenant, const std::string& database, int64_t id, bool retry);
  class KeyStore& keys() { return *keys_; }

  // Waits until queued mail has been handed to the mail server (tests).
  void drain_mail();

  // Backups to S3 (design/machine/019.Backups.md). A host thread ticks every few seconds;
  // tests turn that off with set_backups_manual(true) before start() and tick themselves.
  void set_backups_manual(bool manual) { backups_manual_ = manual; }
  void backup_tick(int64_t now_ms);
  // Runs "nightly", "increment", or "assets" now. nullopt with `error` when it couldn't.
  std::optional<boost::json::object> backup_run(const std::string& what, int64_t now_ms, std::string* error);
  boost::json::object backup_status(bool with_points, int64_t now_ms = 0);  // 0: now

  // For tests and tools.
  config::ConfigHolder& config() { return config_; }
  content::ContentStore& content() { return *content_; }
  site::Site& site() { return *site_; }
  attachments::Store& attachments() { return *attachments_; }
  db::SchemaStore& schemas() { return *schemas_; }
  db::Databases& databases() { return *databases_; }
  boxes::BoxRegistry& boxes() { return *boxes_; }
  identity::Identity& identity() { return *identity_; }
  mail::templates::Store& mail_templates() { return *mail_templates_; }
  const std::vector<std::string>& startup_notes() const { return startup_notes_; }
  // The operator panel's rollups and change log (design/machine/028.Panel.md).
  operations::Traffic& traffic() { return *traffic_; }
  operations::ChangeLog& changes() { return *changes_; }
  logs::Set& logs() { return *logs_; }
  operations::LiveStats& live() { return live_; }
  // Whether the host is ready (GET /goober/ready):
  // storage writable (when probed), identity answering, boxes loaded, the queue not full.
  std::pair<int, boost::json::object> readiness(bool probe_storage);
  // The panel's own listener (operations.listen), or 0 when there is none.
  uint16_t panel_port() const;
  // Tools that read and change nothing: all a host:observer, and the panel, may call (§ 7).
  static bool is_read_tool(std::string_view tool);
  // Operator scope (design/machine/030.OperatorScope.md): which tenants an operator acts on.
  enum class ScopeRule { kTenant, kBox, kManifest, kDomain, kSchema, kProject, kProjectCreate, kTicket, kResult,
                         kFiltered, kOpen, kHost };
  struct OperatorScope {
    bool all = true;
    std::set<std::string> tenants;  // when not all
    std::string set_by;
    int64_t at_ms = 0;
  };
  // Every operator tool's rule; nullopt for a tool that has none (refused when focused).
  static std::optional<ScopeRule> scope_rule(std::string_view tool);
  // Whose scope an operator name uses: "person:$email" for the panel and OAuth agents alike,
  // else the name ("config", "token:$name").
  static std::string scope_key(const std::string& operator_name);
  std::map<std::string, OperatorScope> operator_scopes() const;
  OperatorScope operator_scope(const std::string& operator_name) const;
  // Writes one operator's scope (all removes the entry). Refuses tenants the config doesn't serve.
  bool set_operator_scope(const std::string& key, const OperatorScope& scope, const std::string& by, std::string* why);
  // The MCP policy for an operator: read-only for an observer, and their scope's checks and filters.
  // hold: when approvals are on for the operator (035), changing calls wait for a person.
  mcp::McpServer::Policy operator_policy(const std::string& operator_name, bool read_only, bool hold = true);
  // The operator tool server, for tests that check every tool has a scope rule.
  const mcp::McpServer& operator_tools() const { return *mcp_; }
  // One step of every running crawl: at most one page each (design/machine/017.Crawl.md).
  // The crawler thread calls it; tests call it directly. Returns the pages fetched.
  int crawl_step();

 private:
  void handle(server::http::Request req, server::http::Responder respond, bool panel_listener = false);
  // The operator panel (panel.cpp): its origin is operations.domain or its own listener.
  bool is_panel_request(const server::http::Request& req, bool panel_listener) const;
  void handle_panel(const server::http::Request& req, const Finish& finish, const std::string& path,
                    const std::string& domain);
  void register_panel_tools();
  void register_library_tools();
  // An operator's host roles: tokens are host:owner; an OAuth operator's come from the config.
  std::vector<std::string> operator_roles(const std::string& operator_name) const;
  static bool may_change(const std::vector<std::string>& roles);
  // The WAL's observer: one change-log row per change, with this thread's operator.
  void record_bundle(const wal::Bundle& bundle);
  void run_traffic();
  // The request id of the operator call running on this thread, for the change log.
  static std::string& current_request_id();
  std::unique_ptr<operations::Traffic> traffic_;
  std::unique_ptr<operations::ChangeLog> changes_;
  std::unique_ptr<server::http::Server> panel_server_;
  std::thread traffic_thread_;
  void handle_operations(const server::http::Request& req, const Finish& finish);
  // The operator a bearer token names ("config" or "token:$name"), or "" when there is none.
  std::string bearer_operator(const server::http::Request& req) const;
  // The host-owned /identity/ endpoints (identity_routes.cpp).
  // On the panel's origin, host_only: only host identities sign in there.
  void handle_identity(const server::http::Request& req, const Finish& finish, const std::string& path,
                       const std::string& domain, const std::string& tenant, bool host_only = false);
  // A bearer access token, or else the browser's session cookie.
  std::optional<SignedIn> signed_in(const server::http::Request& req, const std::string& tenant);
  // The session cookie only: what the sign-in, consent, and device screens trust.
  std::optional<SignedIn> browser_session(const server::http::Request& req, const std::string& tenant);
  // The page shell of every identity and OAuth screen: no framing, no scripts, forms to
  // this site (plus `form_action_extra`, for the consent screen's redirect).
  server::http::Response identity_page(const std::string& tenant, int status, const std::string& title,
                                       const std::string& body, const std::string& form_action_extra = "",
                                       bool tenant_css = true) const;
  // The host-owned OAuth endpoints: /.well-known/oauth-authorization-server and /oauth/ (oauth_routes.cpp).
  void handle_oauth(const server::http::Request& req, const Finish& finish, const std::string& path,
                    const std::string& domain, const std::string& tenant);
  // Mails the person that an app or agent was just allowed to act for them (013).
  void mail_new_grant(const SignedIn& who, const identity::Client& client, const std::string& domain,
                      const std::string& from);
  // A client from the realm, or from identity.clients in the config (registered on first use).
  std::optional<identity::Client> oauth_client(const std::string& tenant, const std::string& client_id);
  // The client's address: the peer, or the last X-Forwarded-For entry behind a proxy.
  std::string client_address(const server::http::Request& req) const;
  // This request's origin, like https://example.com or http://localhost:8080.
  std::string origin_of(const server::http::Request& req) const;
  std::string session_cookie_name() const;
  void send_mail(const identity::Mail& mail);
  void write_dev_mail(const identity::Mail& mail);
  struct BoxCall {
    runtime::wasm::CallResult result;
    boost::json::object extra;  // fields for the access log
  };
  // Optional parts of a box call: a timer's own timeout, and host work that must commit
  // with the box's writes (a one-shot timer turning its row off), run only on success.
  struct CallOptions {
    std::optional<int64_t> timeout_ms;
    std::function<bool(db::Session&, std::string*)> before_commit;
    bool no_response_ok = false;  // a timer fire may just return: there is nobody to answer
    bool read_only = false;       // a subscription's evaluation (036 § 5): no writes, no effects
  };
  BoxCall call_box(const boxes::RouteRef& ref, const boost::json::object& doc, const std::string& tenant,
                   const std::string& request_id, const std::optional<SignedIn>& who, const CallOptions& options);
  BoxCall call_box(const boxes::RouteRef& ref, const boost::json::object& doc, const std::string& tenant,
                   const std::string& request_id, const std::optional<SignedIn>& who) {
    return call_box(ref, doc, tenant, request_id, who, CallOptions());
  }
  // /mcp on every domain: box tools for agents and people with bearer tokens (mcp_routes.cpp).
  void handle_tenant_mcp(const server::http::Request& req, const Finish& finish, const std::string& domain,
                         const std::string& tenant);
  void serve_box(server::http::Request req, Finish finish, boxes::RouteRef ref,
                 std::map<std::string, std::string> params, std::string domain, std::string tenant,
                 std::shared_ptr<const boxes::ActiveSet> keep_alive);
  // Content: pages, templates, listings, and static files (007.Content.md).
  void serve_content(const server::http::Request& req, const Finish& finish, const std::string& tenant,
                     const std::string& path);
  // The host-owned /attachment/ endpoints (attachment_routes.cpp).
  void handle_attachment(const server::http::Request& req, const Finish& finish, const std::string& path,
                         const std::string& tenant);
  // Who is calling, or nullopt; throws only when the identity store fails.
  std::optional<SignedIn> caller(const server::http::Request& req, const std::string& tenant);
  // The request document's "user": null, or the signed-in person.
  static boost::json::value user_json(const std::optional<SignedIn>& who);
  // Checks requirements written as JSON (a page's front matter): 200, 401, 403, or 400.
  static int authorize(const boost::json::value& requirements, const std::optional<SignedIn>& who,
                       const std::string& tenant);
  // 303 to sign in for a browser GET, else the 401 page.
  server::http::Response sign_in_or_401(const server::http::Request& req, const std::string& tenant) const;
  server::http::Response error_page(const std::string& tenant, int status, const std::string& detail) const;
  void register_tools();
  void register_content_tools();
  void register_data_tools();
  // The upload check for a manifest's data: schemas stored, grants within them, and
  // every tenant's existing databases able to take them (008 §3).
  bool check_data(const manifest::Manifest& m, std::string* error) const;
  // Schemas a deploy is about to store, seen by check_data on this thread only, so a project's
  // boxes are checked against its new schemas before anything is stored (source_deploy).
  static thread_local const std::map<std::string, db::Schema>* pending_schemas_;
  std::optional<db::Schema> schema_named(const std::string& name) const;
  bool check_calls(const manifest::Manifest& m, std::string* error) const;
  // The operator behind the MCP call running on this thread ("config" or "token:$name").
  static std::string& current_operator();
  // Sets the operator and request id for this thread, and puts the old ones back however the
  // scope ends (an exception included), so a pool thread never keeps an agent's name.
  struct OperatorContext {
    OperatorContext(Host&, const std::string& op, const std::string& request_id)
        : saved_op_(current_operator()), saved_req_(current_request_id()) {
      current_operator() = op;
      current_request_id() = request_id;
    }
    ~OperatorContext() {
      current_operator() = saved_op_;
      current_request_id() = saved_req_;
    }
    OperatorContext(const OperatorContext&) = delete;
    OperatorContext& operator=(const OperatorContext&) = delete;

   private:
    std::string saved_op_, saved_req_;
  };
  void access_log(const server::http::Request& req, int status, int64_t started_us, const std::string& domain,
                  const std::string& extra);

  std::filesystem::path config_file_;
  config::ConfigHolder config_;
  std::unique_ptr<wal::HostWal> wal_;
  std::unique_ptr<content::ContentStore> content_;
  std::unique_ptr<site::Site> site_;
  std::unique_ptr<attachments::Store> attachments_;
  std::unique_ptr<db::SchemaStore> schemas_;
  std::unique_ptr<db::Databases> databases_;
  std::unique_ptr<runtime::wasm::Engine> engine_;
  std::unique_ptr<boxes::BoxRegistry> boxes_;
  std::unique_ptr<identity::Identity> identity_;
  std::unique_ptr<mcp::McpServer> mcp_;
  std::unique_ptr<WorkerPool> pool_;
  std::unique_ptr<server::http::Server> server_;
  std::vector<std::string> startup_notes_;
  int64_t started_ms_ = 0;
  std::atomic<uint64_t> requests_{0};
  RateLimiter limiter_;
  std::unique_ptr<class KeyStore> keys_;
  std::thread outbox_thread_;
  void run_outbox();
  std::vector<std::pair<std::string, std::string>> outbox_locations();
  int outbox_deliver(const std::string& tenant, const std::string& database, int64_t now_ms);
  std::mutex outbox_scan_mu_;
  int64_t outbox_scan_ms_ = 0;
  std::set<std::pair<std::string, std::string>> outbox_scanned_;
  std::unique_ptr<sqlite::Db> open_outbox(const std::string& tenant, const std::string& database);
  // 035 § results: an element's outcome, as a request to the box's routes.result route.
  // Calls a box's routes.result route with an outcome, as nobody or as a person; true when it took it.
  bool deliver_result(const std::string& tenant, const std::string& box, const std::string& route,
                      const boost::json::object& outcome, const std::optional<SignedIn>& who = std::nullopt);
  bool enqueue_outbox(const manifest::Manifest& m, const std::string& tenant, std::vector<struct OutboxElement>& pending,
                      db::Session* session, std::string* error);
  std::string external_call(struct CallContext& ctx, std::string_view service, std::string_view method,
                            std::string_view request);
  std::string internal_call(struct CallContext& ctx, const manifest::ExternalService& svc,
                            const manifest::ExternalMethod& m, const boost::json::value& request);
  void egress_log(const std::string& tenant, const std::string& box, const std::string& service,
                  const std::string& method, const std::string& verb, const std::string& url, int status,
                  const std::string& error, int64_t us, size_t bytes_out, size_t bytes_in,
                  const std::string& request_id, int64_t outbox_id);
  struct TimerState;
  std::unique_ptr<TimerState> timers_;
  std::thread timer_thread_;
  std::mutex timer_thread_mu_;
  std::condition_variable timer_cv_;
  bool timers_stopping_ = false;
  void run_timers();
  sqlite::Db& timer_progress(const std::string& tenant);
  std::function<bool(db::Session&, std::string*)> disable_fired_row(const manifest::DataStrategy* source,
                                                                    const manifest::HttpRoute& t,
                                                                    const boost::json::object& row);
  // The polite robot and its crawls (crawler.cpp, web_fetch.cpp).
  std::unique_ptr<class WebFetcher> web_;
  std::thread crawl_thread_;
  void init_web();
  void register_web_tools();
  void run_crawls();
  std::string web_call(struct CallContext& ctx, const manifest::ExternalService& svc, const boost::json::value& request);
  // A box's mail (design/machine/018.Mail.md): checked now, sent after commit through the outbox.
  // The daily cap on a tenant's outbox calls to one service (config limits.outbox-daily-per-service).
  bool outbox_quota_take(const std::string& tenant, const std::string& service, std::string* why);
  std::mutex outbox_quota_mu_;
  std::string mail_call(struct CallContext& ctx, const manifest::ExternalService& svc,
                        const manifest::ExternalMethod& m, const boost::json::value& request);
  // Mail in (mail_in.cpp; design/machine/024.MailIn.md): Stalwart's MTA hook, mail routes,
  // reply tokens, and received attachments.
 public:
  struct MailMatch {
    boxes::RouteRef ref;
    std::string tenant, domain, to;
    std::map<std::string, std::string> params;  // from the address, and a token's claims
    std::vector<std::string> unused;
    std::string reply_recipient;  // for a token: who the reply address was sent to
  };
  // The mail route that claims an address, with its params; nullopt with `why` when none does
  // (or its token is forged or expired).
  std::optional<MailMatch> match_mail(const std::string& address, std::string* why);
  int prune_mail(int64_t now_ms);

 private:
  void handle_mail_hook(const server::http::Request& req, const Finish& finish);
  std::string reply_key();
  std::unique_ptr<sqlite::Db> open_mail_db();
  std::optional<std::string> mint_reply_address(const struct CallContext& ctx, const boost::json::object& reply,
                                                const std::string& recipient, std::string* error);
  std::string mail_attachment_call(struct CallContext& ctx, const boost::json::value& request);
  // The sender as a verified claim (038 § The sender is a verified claim), or "".
  std::string verified_sender(const boost::json::object& hook_request, const struct mail::inbound::Message& message,
                              const std::string& envelope_from);
  std::mutex mail_in_mu_;
  // Delivers one outbox mail element through the mailer, within the config's mail limits.
  struct HttpOut deliver_outbox_mail(const std::string& tenant, const std::string& body, int64_t now_ms);
  size_t max_queued_ = 1024;  // box jobs waiting for a thread, past which requests get 503
  // One-time upload URLs (attachment_upload_url): a ticket, kept by its hash, lets one PUT or
  // POST to /attachment/upload store one attachment in one tenant, for 15 minutes.
 public:
  struct UploadTicket {
    std::string tenant, name, alt, visibility, by;
    int64_t expires_ms = 0;
    std::string parts = {};  // bytes sent so far by attachment_upload_part
    int part_count = 0;
  };
  std::string issue_upload_ticket(UploadTicket t);
  // The ticket's terms if it is live and for this tenant; with consume, it can't be used again.
  std::optional<UploadTicket> upload_ticket(const std::string& ticket, const std::string& tenant, bool consume);
  // The tenant a live ticket is for, without using it.
  std::optional<std::string> upload_ticket_tenant(const std::string& ticket);
  std::optional<std::string> scope_refusal(const OperatorScope& scope, const std::string& operator_name,
                                           const std::string& tool, const boost::json::object& args);
  std::optional<std::string> scope_filter(const OperatorScope& scope, const std::string& operator_name,
                                          const std::string& tool, boost::json::value& result);
  std::mutex scopes_mu_;
  void register_scope_tools();
  // Speaking HTTP (http_out.cpp, connect_routes.cpp; design/machine/034.SpeakingHttp.md).
  struct Secret {
    std::string kind, value, token, token_secret, owner;
    std::string origin;  // a sealed secret's: the only origin it may be sent to (scheme://host[:port])
  };
  struct HttpxCall {
    std::string verb, url, body, content_type;
    std::vector<std::pair<std::string, std::string>> headers, form;
  };
  struct FileBytes {
    std::string filename, content_type, bytes;
  };
  static bool uses_httpx(const manifest::ExternalService& s, const manifest::ExternalMethod& m);
  std::optional<vault::Key> vault_tenant_key(const std::string& tenant, std::string* error);
  std::string seal_secret(const std::string& tenant, const std::string& service, const std::string& owner,
                          const std::string& plaintext, std::string* error);
  std::optional<Secret> open_secret(const std::string& tenant, const manifest::ExternalService& svc, const std::string& sealed,
                                    std::string* resealed, std::string* error);
  std::optional<Secret> secret_for(const std::string& tenant, const manifest::ExternalService& svc,
                                   const boost::json::object& request, const std::string& url, std::string* resealed,
                                   std::string* error);
  std::optional<boost::json::object> oauth2_token(const std::string& tenant, const manifest::ExternalService& svc,
                                                  std::vector<std::pair<std::string, std::string>> form, std::string* error);
  std::optional<HttpxCall> build_httpx(const std::string& tenant, const boost::json::value& user, const std::string& domain,
                                       const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                                       const std::string& op_ref, const boost::json::object& request,
                                       const std::string& body_mode, std::string* error);
  std::optional<FileBytes> attachment_bytes(const std::string& tenant, const boost::json::value& id, std::string* error);
  bool apply_secret(const std::string& tenant, const manifest::ExternalService& svc, const Secret& secret, HttpxCall* call,
                    std::string* error);
  struct HttpOut send_httpx(const std::string& tenant, const std::string& box, const manifest::ExternalService& svc,
                            const manifest::ExternalMethod& m, const HttpxCall& call, int64_t timeout_ms,
                            const std::string& request_id, int64_t outbox_id);
  static boost::json::object answer_of(const struct HttpOut& out);
  std::string httpx_call(struct CallContext& ctx, const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                         const boost::json::value& request);
  struct HttpOut deliver_outbox_httpx(const std::string& tenant, sqlite::Db& db, int64_t id, const std::string& box,
                                      const std::string& service, const std::string& method, const std::string& body,
                                      const std::string& idem_header, const std::string& idem_key, int64_t timeout_ms,
                                      int64_t now_ms);
  sqlite::Db& connections_db();
  void register_spec_tools();
  // Approvals per connected agent (design/machine/035.Approvals.md).
  sqlite::Db& approvals_db();
  bool approvals_on(const std::string& agent);
  bool agent_still_signs_in(const std::string& agent);
  void saw_agent(const std::string& agent);
  std::optional<mcp::ToolResult> hold_for_approval(const std::string& agent, const std::string& tool,
                                                   const boost::json::object& args);
  static boost::json::object approval_json(sqlite::Stmt& s, bool with_args);
  boost::json::object panel_approvals(const SignedIn& who);
  std::pair<int, boost::json::object> panel_set_approvals(const SignedIn& who, const boost::json::object& args,
                                                          const std::string& request_id);
  std::pair<int, boost::json::object> panel_decide(const SignedIn& who, const boost::json::object& args,
                                                   const std::string& request_id);
  void register_approval_tools();
  // The bus and subscriptions (040; design/machine/036.Bus.md; sse.cpp).
  struct SseSub {
    std::shared_ptr<server::http::Stream> stream;
    std::string client;
    int64_t opened_ms = 0, last_sent = -1;
    bool has_answer = false;
  };
  struct SseGroup {
    std::string key, tenant, domain, path, box, route;
    std::optional<SignedIn> who;
    boost::json::object doc;
    std::vector<std::string> topics;
    int64_t coalesce_ms = 250, refresh_ms = 30000;
    std::vector<std::shared_ptr<SseSub>> subs;
    uint64_t watch_id = 0;
    int64_t last_eval_ms = 0, due_ms = 0, last_id = -1, evaluations = 0;
    std::string last_body;
    bool running = false;
  };
  static std::optional<std::string> resolve_topic(const std::string& tmpl, const std::string& tenant,
                                                  const boost::json::object& doc, const std::optional<SignedIn>& who);
  void serve_subscription(const server::http::Request& req, const Finish& finish, boxes::RouteRef ref,
                          std::map<std::string, std::string> params, const std::string& domain, const std::string& tenant,
                          bool stream);
  void sse_evaluate(std::shared_ptr<SseGroup> g);
  void sse_loop();
  void sse_stop();
  boost::json::object bus_status(const std::string& tenant);
  bus::Bus bus_{base_seed()};
  static int64_t base_seed();
  std::mutex sse_mu_;
  std::condition_variable sse_cv_;
  std::map<std::string, std::shared_ptr<SseGroup>> sse_groups_;
  std::thread sse_thread_;
  bool sse_stopping_ = false;
  std::mutex approvals_mu_;
  std::unique_ptr<sqlite::Db> approvals_db_;
  std::map<std::string, int64_t> agents_seen_;
  void handle_connect(const server::http::Request& req, const Finish& finish, const std::string& path,
                      const std::string& domain, const std::string& tenant);
  std::unique_ptr<specs::Store> specs_;
  std::mutex vault_mu_;
  std::map<std::string, std::mutex> refresh_locks_;
  std::unique_ptr<sqlite::Db> connections_db_;
  // Logs (logs_tools.cpp; design/machine/032.Logs.md): every log line goes through log_line,
  // which hashes addresses and writes the envelope and chain; audit() writes the audit log.
  void log_line(const std::string& log, boost::json::object line);
  void audit(const std::string& action, const std::string& actor, const std::string& target, const std::string& outcome,
             boost::json::object more = {});
  std::string address_hash_for_logs(const std::string& address);
  void register_log_tools();
  // Hourly: rotate quiet logs, and keep 7 days of rotated files on the machine (§ 2).
  void logs_housekeeping(int64_t now_ms);
  std::unique_ptr<logs::Set> logs_;
  operations::LiveStats live_;
  // The content kit (content_tools.cpp; design/machine/031.ContentKit.md).
  void register_content_kit_tools();
  // GET a path of one of this host's sites through its own listener, signed out.
  struct HttpOut fetch_self(const std::string& domain, const std::string& path);
  // The panel's Operators view, and its two writes (030 § 2; 029 § 6).
  boost::json::object panel_operators(const SignedIn& who);
  std::pair<int, boost::json::object> panel_set_scope(const SignedIn& who, const boost::json::object& args,
                                                      const std::string& request_id);
  std::pair<int, boost::json::object> panel_unblock(const SignedIn& who, const boost::json::object& args,
                                                    const std::string& request_id);

 private:
  std::mutex upload_tickets_mu_;
  std::map<std::string, UploadTicket> upload_tickets_;
  uint64_t upload_part_bytes_ = 0;  // held across every ticket's parts, at most 64 MiB
  std::unique_ptr<class MailQueue> mail_;
  // Every mail the host sends is rendered from these (design/machine/022.MailTemplates.md).
  std::unique_ptr<mail::templates::Store> mail_templates_;
  void register_mail_tools();
  // Who a tenant may mail (consent.cpp; design/machine/029.Invites.md §§ 3–6): blocks, lists,
  // categories, the host's footer, and the preferences page every footer links to.
 public:
  // Whether the address is blocked for the tenant: it declined an invite, unsubscribed, or an
  // operator blocked it. Every box mail and invite is refused.
  bool mail_suppressed(const std::string& tenant, const std::string& address);
  mail::consent::Store& consent() { return *consent_; }

 private:
  std::unique_ptr<mail::consent::Store> consent_;
  std::mutex sent_mu_;  // storage/mail/sent.db, the daily mail ledger
  // The site a tenant's links name: its first domain that isn't localhost or an address.
  std::string site_of(const std::string& tenant) const;
  // The preferences link for an address: /goober/mail/$tenant/$hash/$mac[/$scope], where scope
  // is "" (everything), or "list.$name" for a list's one-click unsubscribe.
  std::string mail_link(const std::string& domain, const std::string& tenant, const std::string& address,
                        const std::string& scope);
  std::string unsubscribe_url(const std::string& domain, const std::string& tenant, const std::string& address) {
    return mail_link(domain, tenant, address, "");
  }
  // The lists a tenant's boxes declare: name -> {title, about, box}.
  std::map<std::string, std::pair<manifest::ExternalService::MailList, std::string>> tenant_lists(const std::string& tenant);
  // A box mail's category (§ 3): `category` as given (or "" to infer), checked for this address.
  // Returns the category, or nullopt with `why`. At delivery, consent is checked again.
  struct MailCheck {
    std::string category, list, relationship;  // relationship: account, wrote_in, or ""
  };
  std::optional<MailCheck> check_mail_category(const std::string& tenant, const std::string& to, std::string category,
                                               const std::string& list, const std::string& in_reply_to, bool person_present,
                                               bool at_delivery, std::string* why);
  bool wrote_in(const std::string& tenant, const std::string& address, int64_t within_ms);
  bool thread_verified(const std::string& tenant, const std::string& address, const std::string& message_id);
  // The host's footer, added after any template or layout: why the mail came, the postal
  // address, and the link to stop it.
  void add_footer(std::string* text, std::string* html, const std::string& why, const std::string& postal,
                  const std::string& link_label, const std::string& link);
  std::string footer_reason(const MailCheck& c, const std::string& site, const std::string& list_title) const;
  void handle_unsubscribe(const server::http::Request& req, const Finish& finish, const std::string& path,
                          const std::string& tenant);
  void handle_list_confirm(const server::http::Request& req, const Finish& finish, const std::string& path,
                           const std::string& tenant);
  void register_mail_out_tools();
  void register_consent_tools();
  // The mail service's subscribe and preferences methods (§ 4).
  std::string mail_subscribe_call(struct CallContext& ctx, const manifest::ExternalService& svc,
                                  const boost::json::value& request);
  std::string mail_preferences_call(struct CallContext& ctx, const boost::json::value& request);
  struct HttpOut deliver_list_confirm(const std::string& tenant, const std::string& body, int64_t now_ms);
  // The account page's Mail section, and its posts (lists on and off, lifting a block).
  std::string account_mail_section(const std::string& tenant, const std::string& email);
  bool account_mail_post(const std::string& tenant, const SignedIn& who, const std::string& path,
                         const std::map<std::string, std::string>& form, const std::string& request_id, std::string* why);

  // Invites (invites.cpp; 029 § 2).
  void register_invite_tools();
  std::string invites_call(struct CallContext& ctx, const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                           const boost::json::value& request);
  struct HttpOut deliver_outbox_invite(const std::string& tenant, const std::string& body, const std::string& key,
                                       int64_t now_ms);
  // Checks an invite against blocks, the host config, and the limits, then stores and mails it.
  std::optional<identity::Invite> make_invite(const std::string& tenant, identity::Invite invite, std::string* code,
                                              std::string* why);
  bool send_invite_mail(const std::string& tenant, const identity::Invite& invite, bool reminder, std::string* why);
  std::string invite_mac(const std::string& tenant, int64_t id, const std::string& email);
  std::string invite_url(const std::string& tenant, const identity::Invite& invite, bool decline);
  void handle_invite(const server::http::Request& req, const Finish& finish, const std::string& path,
                     const std::string& domain, const std::string& tenant);
  // Accepts an invite for a signed-in person: roles, terms, lists. Returns where to go next.
  std::optional<std::string> accept_invite_for(const std::string& tenant, const identity::Invite& invite,
                                               const identity::User& user, const std::string& how,
                                               const std::string& request_id, std::string* why);
  // Hourly: reminders due (one per invite).
  void remind_invites(int64_t now_ms);
  // The tenant's terms page and its hash now, or nullopt when the tenant has none.
  std::optional<std::pair<std::string, std::string>> terms_now(const std::string& tenant);
  // Progressive web apps (pwa_routes.cpp; design/machine/782.PWA.md): /goober/pwa/$app/,
  // the pwa_* tools, and Web Push through the outbox.
  void handle_pwa(const server::http::Request& req, const Finish& finish, const std::string& path,
                  const std::string& tenant);
  void register_pwa_tools();
  std::optional<webpush::Key> vapid_key(std::string* error);
  std::mutex vapid_mu_;
  std::unique_ptr<sqlite::Db> open_push_db();
  bool push_endpoint_allowed(const std::string& endpoint, bool* loopback, std::string* why) const;
  std::string vapid_contact() const;
  struct HttpOut deliver_push(const std::string& tenant, const std::string& app, const boost::json::object& to,
                              const std::string& payload, const boost::json::object& options, const std::string& key,
                              int64_t now_ms, boost::json::object* report);
  struct HttpOut deliver_outbox_push(const std::string& tenant, const std::string& body, const std::string& key,
                                     int64_t now_ms);
  std::string push_call(struct CallContext& ctx, const manifest::ExternalService& svc, const manifest::ExternalMethod& m,
                        const boost::json::value& request);
  // Source projects and their builds (source_tools.cpp, builder.cpp).
  std::unique_ptr<source::Store> source_;
  void register_source_tools();
  void run_source_job(struct BuildJob& job, bool deploy);
  std::unique_ptr<class BackupService> backups_;
  std::thread backup_thread_;
  bool backups_manual_ = false;
  void run_backups();
  // Last, so it is destroyed first: no build runs while the rest of the host goes away.
  std::unique_ptr<class BuildJobs> jobs_;
};

}  // namespace goober::host
