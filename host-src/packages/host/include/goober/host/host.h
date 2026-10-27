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
#include "goober/identity/identity.h"
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

  // For tests and tools.
  config::ConfigHolder& config() { return config_; }
  content::ContentStore& content() { return *content_; }
  site::Site& site() { return *site_; }
  attachments::Store& attachments() { return *attachments_; }
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
  std::optional<SignedIn> signed_in(const server::http::Request& req, const std::string& tenant);
  std::string session_cookie_name() const;
  void send_mail(const identity::Mail& mail);
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
  void access_log(const server::http::Request& req, int status, int64_t started_us, const std::string& domain,
                  const std::string& extra);

  std::filesystem::path config_file_;
  config::ConfigHolder config_;
  std::unique_ptr<wal::HostWal> wal_;
  std::unique_ptr<content::ContentStore> content_;
  std::unique_ptr<site::Site> site_;
  std::unique_ptr<attachments::Store> attachments_;
  std::unique_ptr<runtime::wasm::Engine> engine_;
  std::unique_ptr<boxes::BoxRegistry> boxes_;
  std::unique_ptr<identity::Identity> identity_;
  std::unique_ptr<mcp::McpServer> mcp_;
  std::unique_ptr<WorkerPool> pool_;
  std::unique_ptr<server::http::Server> server_;
  std::vector<std::string> startup_notes_;
  int64_t started_ms_ = 0;
  std::atomic<uint64_t> requests_{0};
};

}  // namespace goober::host
