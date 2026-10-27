#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "goober/attachments/attachments.h"
#include "goober/boxes/registry.h"
#include "goober/config/config.h"
#include "goober/content/content.h"
#include "goober/db/database.h"
#include "goober/identity/identity.h"
#include "goober/host/rate_limit.h"
#include "goober/host/worker_pool.h"
#include "goober/mcp/mcp.h"
#include "goober/runtime_wasm/engine.h"
#include "goober/server_http/server.h"
#include "goober/site/site.h"
#include "goober/wal/wal.h"

namespace goober::host {

inline constexpr std::string_view kVersion = "0.1.0";
inline constexpr std::string_view kOperationsMcpPath = "/goober-operations-mcp";

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

  // Waits until queued mail has been handed to the mail server (tests).
  void drain_mail();

  // For tests and tools.
  config::ConfigHolder& config() { return config_; }
  content::ContentStore& content() { return *content_; }
  site::Site& site() { return *site_; }
  attachments::Store& attachments() { return *attachments_; }
  db::SchemaStore& schemas() { return *schemas_; }
  db::Databases& databases() { return *databases_; }
  boxes::BoxRegistry& boxes() { return *boxes_; }
  identity::Identity& identity() { return *identity_; }
  const std::vector<std::string>& startup_notes() const { return startup_notes_; }

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
  BoxCall call_box(const boxes::RouteRef& ref, const boost::json::object& doc, const std::string& tenant,
                   const std::string& request_id, const std::optional<SignedIn>& who);
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
  size_t max_queued_ = 1024;  // box jobs waiting for a thread, past which requests get 503
  std::unique_ptr<class SmtpQueue> smtp_;
};

}  // namespace goober::host
