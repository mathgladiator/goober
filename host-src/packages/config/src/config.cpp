#include "goober/config/config.h"

#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/names.h"
#include "goober/json/json.h"

namespace goober::config {
namespace bj = boost::json;
using goober::json::get_array;
using goober::json::get_bool;
using goober::json::get_int;
using goober::json::get_object;
using goober::json::get_string;

int64_t Limit::resolve(std::optional<int64_t> requested) const {
  if (!requested) return default_value;
  if (*requested < 1) return 1;
  return *requested > max_value ? max_value : *requested;
}

std::optional<std::string> HostConfig::tenant_for_host(std::string_view host) const {
  std::string h = base::to_lower(host);
  if (!h.empty() && h.front() == '[') {
    auto close = h.find(']');
    if (close != std::string::npos) h = h.substr(0, close + 1);
  } else if (auto colon = h.rfind(':'); colon != std::string::npos) {
    h = h.substr(0, colon);
  }
  if (!h.empty() && h.back() == '.') h.pop_back();
  auto it = domains.find(h);
  if (it == domains.end()) return std::nullopt;
  return it->second.tenant;
}

namespace {

bool check_keys(const bj::object& obj, std::initializer_list<std::string_view> allowed, std::string_view where,
                std::string* error) {
  for (const auto& kv : obj) {
    bool ok = false;
    for (auto a : allowed) ok = ok || kv.key() == a;
    if (!ok) {
      *error = std::string(where) + ": unknown field \"" + std::string(kv.key()) + "\"";
      return false;
    }
  }
  return true;
}

bool read_limit(const bj::object& limits, std::string_view key, Limit& out, std::string* error) {
  const bj::object* obj = get_object(limits, key, error);
  if (!error->empty()) return false;
  if (!obj) return true;
  if (!check_keys(*obj, {"default", "max"}, "limits." + std::string(key), error)) return false;
  if (auto v = get_int(*obj, "default", error)) out.default_value = *v;
  if (auto v = get_int(*obj, "max", error)) out.max_value = *v;
  if (!error->empty()) return false;
  if (out.default_value < 1 || out.max_value < out.default_value) {
    *error = "limits." + std::string(key) + ": need 1 <= default <= max";
    return false;
  }
  return true;
}

}  // namespace

std::shared_ptr<const HostConfig> parse_config(std::string_view text, const std::filesystem::path& base_dir,
                                               std::string* error) {
  std::string err;
  auto parsed = goober::json::parse(text, &err);
  if (!parsed) {
    *error = err;
    return nullptr;
  }
  if (!parsed->is_object()) {
    *error = "the config must be a JSON object";
    return nullptr;
  }
  const auto& root = parsed->get_object();
  auto cfg = std::make_shared<HostConfig>();
  if (!check_keys(root, {"listen", "storage", "platform-domain", "domains", "operations", "threads", "limits"},
                  "config", &err)) {
    *error = err;
    return nullptr;
  }

  if (const auto* listen = get_object(root, "listen", &err)) {
    if (!check_keys(*listen, {"address", "port"}, "listen", &err)) return *error = err, nullptr;
    if (auto v = get_string(*listen, "address", &err)) cfg->listen_address = *v;
    if (auto v = get_int(*listen, "port", &err)) {
      if (*v < 0 || *v > 65535) return *error = "listen.port must be 0 (any free port) to 65535", nullptr;
      cfg->listen_port = uint16_t(*v);
    }
  }
  if (auto v = get_string(root, "storage", &err)) cfg->storage = *v;
  if (cfg->storage.is_relative()) cfg->storage = base_dir / cfg->storage;
  cfg->storage = cfg->storage.lexically_normal();
  if (auto v = get_string(root, "platform-domain", &err)) cfg->platform_domain = base::to_lower(*v);

  if (const auto* domains = get_object(root, "domains", &err)) {
    for (const auto& kv : *domains) {
      std::string domain = base::to_lower(kv.key());
      if (!kv.value().is_object()) return *error = "domains." + domain + " must be an object", nullptr;
      const auto& entry = kv.value().get_object();
      if (!check_keys(entry, {"tenant"}, "domains." + domain, &err)) return *error = err, nullptr;
      auto tenant = get_string(entry, "tenant", &err);
      if (!tenant || !base::is_valid_name(*tenant)) {
        return *error = "domains." + domain + ".tenant must match [a-z][a-z0-9_]*", nullptr;
      }
      cfg->domains[domain] = DomainEntry{*tenant};
    }
  }
  if (const auto* ops = get_object(root, "operations", &err)) {
    if (!check_keys(*ops, {"token"}, "operations", &err)) return *error = err, nullptr;
    if (auto v = get_string(*ops, "token", &err)) cfg->operations_token = *v;
  }
  if (cfg->operations_token.size() < 16) {
    *error = "operations.token must be at least 16 characters";
    return nullptr;
  }
  if (const auto* threads = get_object(root, "threads", &err)) {
    if (!check_keys(*threads, {"network", "boxes", "pin-boxes"}, "threads", &err)) return *error = err, nullptr;
    if (auto v = get_int(*threads, "network", &err)) cfg->network_threads = int(*v);
    if (auto v = get_int(*threads, "boxes", &err)) cfg->box_threads = int(*v);
    if (auto v = get_bool(*threads, "pin-boxes", &err)) cfg->pin_boxes = *v;
    if (cfg->network_threads < 1 || cfg->box_threads < 1) {
      return *error = "threads.network and threads.boxes must be at least 1", nullptr;
    }
  }
  if (const auto* limits = get_object(root, "limits", &err)) {
    if (!check_keys(*limits, {"body-max-bytes", "timeout-ms", "fuel", "memory-max-bytes"}, "limits", &err)) {
      return *error = err, nullptr;
    }
    if (auto v = get_int(*limits, "body-max-bytes", &err)) cfg->body_max_bytes = *v;
    if (!read_limit(*limits, "timeout-ms", cfg->timeout_ms, &err) || !read_limit(*limits, "fuel", cfg->fuel, &err) ||
        !read_limit(*limits, "memory-max-bytes", cfg->memory_max_bytes, &err)) {
      return *error = err, nullptr;
    }
  }
  if (!err.empty()) {
    *error = err;
    return nullptr;
  }
  return cfg;
}

std::shared_ptr<const HostConfig> load_config(const std::filesystem::path& file, std::string* error) {
  auto text = base::read_file(file);
  if (!text) {
    *error = "cannot read " + file.string();
    return nullptr;
  }
  auto base_dir = std::filesystem::absolute(file).parent_path();
  std::string err;
  auto cfg = parse_config(*text, base_dir, &err);
  if (!cfg) *error = file.string() + ": " + err;
  return cfg;
}

ConfigHolder::ConfigHolder(std::shared_ptr<const HostConfig> initial) : current_(std::move(initial)) {}

std::shared_ptr<const HostConfig> ConfigHolder::get() const {
  std::lock_guard lock(mu_);
  return current_;
}

std::string ConfigHolder::replace(std::shared_ptr<const HostConfig> next) {
  std::lock_guard lock(mu_);
  auto merged = std::make_shared<HostConfig>(*next);
  std::string notes;
  auto keep = [&](auto& field, const auto& old, const char* name) {
    if (field != old) {
      notes += std::string(name) + " changed; it takes effect after a restart. ";
      field = old;
    }
  };
  keep(merged->listen_address, current_->listen_address, "listen.address");
  keep(merged->listen_port, current_->listen_port, "listen.port");
  keep(merged->storage, current_->storage, "storage");
  keep(merged->network_threads, current_->network_threads, "threads.network");
  keep(merged->box_threads, current_->box_threads, "threads.boxes");
  keep(merged->pin_boxes, current_->pin_boxes, "threads.pin-boxes");
  current_ = std::move(merged);
  return notes;
}

}  // namespace goober::config
