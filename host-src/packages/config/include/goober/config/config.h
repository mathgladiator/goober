#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

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
};

// The whole host configuration, from one JSON file (see design/machine/005.FirstVersion.md §6).
struct HostConfig {
  std::string listen_address = "127.0.0.1";
  uint16_t listen_port = 8080;
  std::filesystem::path storage = "./storage";
  std::string platform_domain = "localhost";
  std::map<std::string, DomainEntry> domains;
  std::string operations_token;
  int network_threads = 2;
  int box_threads = 4;
  bool pin_boxes = false;
  int64_t body_max_bytes = 1 << 20;
  Limit timeout_ms{1000, 10000};
  Limit fuel{100'000'000, 2'000'000'000};
  Limit memory_max_bytes{64 << 20, 512 << 20};

  // The tenant for a Host header value, with any port removed and case folded.
  std::optional<std::string> tenant_for_host(std::string_view host) const;
};

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
