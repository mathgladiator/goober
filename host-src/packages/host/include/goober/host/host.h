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
#include "goober/boxes/registry.h"
#include "goober/config/config.h"
#include "goober/content/content.h"
#include "goober/db/database.h"
#include "goober/identity/identity.h"
#include "goober/mail_inbound/mime.h"
#include "goober/mail_templates/templates.h"
#include "goober/host/rate_limit.h"
#include "goober/host/worker_pool.h"
#include "goober/mcp/mcp.h"
#include "goober/runtime_wasm/engine.h"
#include "goober/server_http/server.h"
#include "goober/site/site.h"
#include "goober/source/source.h"
#include "goober/wal/wal.h"

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
  // One step of every running crawl: at most one page each (design/machine/017.Crawl.md).
  // The crawler thread calls it; tests call it directly. Returns the pages fetched.
  int crawl_step();

 private:
  void handle(server::http::Request req, server::http::Responder respond);
  void handle_operations(const server::http::Request& req, const Finish& finish);
  // The operator a bearer token names ("config" or "token:$name"), or "" when there is none.
  std::string bearer_operator(const server::http::Request& req) const;
  // The host-owned /identity/ endpoints (identity_routes.cpp).
  void handle_identity(const server::http::Request& req, const Finish& finish, const std::string& path,
                       const std::string& domain, const std::string& tenant);
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

 private:
  std::mutex upload_tickets_mu_;
  std::map<std::string, UploadTicket> upload_tickets_;
  uint64_t upload_part_bytes_ = 0;  // held across every ticket's parts, at most 64 MiB
  std::unique_ptr<class MailQueue> mail_;
  // Every mail the host sends is rendered from these (design/machine/022.MailTemplates.md).
  std::unique_ptr<mail::templates::Store> mail_templates_;
  void register_mail_tools();
  // Unsubscribes (mail_out.cpp; design/machine/018.Mail.md § 3): a tenant's list of addresses
  // that asked for no more of its boxes' mail, kept by hash, and the one-click link to join it.
 public:
  bool mail_suppressed(const std::string& tenant, const std::string& address);

 private:
  void mail_suppress(const std::string& tenant, const std::string& address_hash, const std::string& how, bool on);
  std::string unsubscribe_url(const std::string& domain, const std::string& tenant, const std::string& address);
  void handle_unsubscribe(const server::http::Request& req, const Finish& finish, const std::string& path,
                          const std::string& tenant);
  void register_mail_out_tools();
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
