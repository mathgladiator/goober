#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goober::config {

// A limit with a host default and a ceiling that no manifest may exceed.
struct Limit {
  int64_t default_value = 0;
  int64_t max_value = 0;
  // The manifest's value clamped to [1, max], or the default when absent.
  int64_t resolve(std::optional<int64_t> requested) const;
};

struct DomainEntry {
  std::string tenant;
  std::string owner;  // the first owner's email, granted "owner" when that address first signs in; or ""
};

// The built-in identity service (013.Identity.md; design/machine/006.LocalIdentity.md).
struct IdentityConfig {
  // Session cookies are Secure and named __Host-goober-session. Off in local mode,
  // since a plain-HTTP host can't set a Secure cookie everywhere.
  bool cookie_secure = true;
  int64_t session_days = 30;
  // "dev" writes each mail to storage/mail/ as a MIME file. "smtp" sends it through an
  // SMTP server with TLS (submission on 587 with STARTTLS, or smtps on 465).
  std::string mailer = "dev";
  struct Smtp {
    std::string url;            // smtps://host:465 or smtp://host:587 (STARTTLS is required)
    std::string from;           // the sender address, like no-reply@example.com
    std::string username;       // may be empty for a server that needs no sign-in
    std::string password_file;  // a file holding only the password; kept out of the config
  };
  Smtp smtp;
  // Host identities: an email and the host roles it holds, like "host:owner".
  std::map<std::string, std::vector<std::string>> operators;
  // OAuth clients registered in every tenant realm, by client_id, like the dumb
  // terminal (999). The identity service checks the redirect URIs when it registers them.
  struct Client {
    std::string name;
    std::vector<std::string> redirect_uris;
    bool person = false;  // "acts-as": "person" or "agent"
  };
  std::map<std::string, Client> clients;
};

// The whole host configuration, from one JSON file (see design/machine/005.FirstVersion.md §6).
struct HostConfig {
  std::string listen_address = "127.0.0.1";
  uint16_t listen_port = 8080;
  std::filesystem::path storage = "./storage";
  std::string platform_domain = "localhost";
  std::map<std::string, DomainEntry> domains;
  // Where people and agents reach this host, like https://goober.example.com. Optional;
  // used to print links. Defaults to http://$listen_address:$listen_port.
  std::string public_url;
  // A bearer token for the operator MCP endpoint, kept in the config. Optional:
  // tokens minted with `goober-host token create` live in storage instead.
  std::string operations_token;
  IdentityConfig identity;
  // Set by --local, never by the file: see local_mode().
  bool local = false;
  int network_threads = 2;
  int box_threads = 4;
  bool pin_boxes = false;
  int64_t body_max_bytes = 1 << 20;
  // Requests one client address may make per minute, in a burst of a quarter of that.
  int64_t requests_per_minute = 1200;
  // The host sits behind a proxy like Caddy (099): the client's address is the last one in
  // X-Forwarded-For. Leave it off when clients connect directly, or they can forge it.
  bool behind_proxy = false;
  Limit timeout_ms{1000, 10000};
  Limit fuel{100'000'000, 2'000'000'000};
  Limit memory_max_bytes{64 << 20, 512 << 20};

  // The configured domain and its tenant for a Host header value, with any port
  // removed and case folded. In local mode, "$domain.localhost" also reaches
  // $domain, and "localhost", "127.0.0.1", "[::1]", or "10.0.2.2" (the Android emulator),
  // when not configured, reaches the
  // platform domain (or else the first domain).
  std::optional<std::pair<std::string, DomainEntry>> resolve_host(std::string_view host) const;
  std::optional<std::string> tenant_for_host(std::string_view host) const;

  // The base URL for links: public_url, or the listen address and port.
  std::string base_url() const;
};

// A copy of the config for playing on one machine: listen on 127.0.0.1, plain
// (not Secure) session cookies, the dev mailer, and the local domain aliases.
std::shared_ptr<const HostConfig> local_mode(std::shared_ptr<const HostConfig> config);

// Parses and validates a config. On failure returns nullptr and sets `error`.
// A relative storage path is resolved against `base_dir`.
std::shared_ptr<const HostConfig> parse_config(std::string_view text, const std::filesystem::path& base_dir,
                                               std::string* error);

std::shared_ptr<const HostConfig> load_config(const std::filesystem::path& file, std::string* error);

// Holds the current config, swappable on reload, so readers always see one whole config.
class ConfigHolder {
 public:
  explicit ConfigHolder(std::shared_ptr<const HostConfig> initial);
  std::shared_ptr<const HostConfig> get() const;
  // Replaces the config. Fields that need a restart keep their old values, and
  // each one that differs is described in the returned list of notes.
  std::string replace(std::shared_ptr<const HostConfig> next);

 private:
  mutable std::mutex mu_;
  std::shared_ptr<const HostConfig> current_;
};

}  // namespace goober::config
