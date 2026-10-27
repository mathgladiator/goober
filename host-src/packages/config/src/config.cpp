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

std::optional<std::pair<std::string, DomainEntry>> HostConfig::resolve_host(std::string_view host) const {
  std::string h = base::to_lower(host);
  if (!h.empty() && h.front() == '[') {
    auto close = h.find(']');
    if (close != std::string::npos) h = h.substr(0, close + 1);
  } else if (auto colon = h.rfind(':'); colon != std::string::npos) {
    h = h.substr(0, colon);
  }
  if (!h.empty() && h.back() == '.') h.pop_back();
  if (auto it = domains.find(h); it != domains.end()) return *it;
  if (!local || domains.empty()) return std::nullopt;
  constexpr std::string_view kSuffix = ".localhost";
  if (h.size() > kSuffix.size() && h.ends_with(kSuffix)) {
    if (auto it = domains.find(h.substr(0, h.size() - kSuffix.size())); it != domains.end()) return *it;
    return std::nullopt;
  }
  if (h == "localhost" || h == "127.0.0.1") {
    if (auto it = domains.find(platform_domain); it != domains.end()) return *it;
    return *domains.begin();
  }
  return std::nullopt;
}

std::optional<std::string> HostConfig::tenant_for_host(std::string_view host) const {
  auto r = resolve_host(host);
  if (!r) return std::nullopt;
  return r->second.tenant;
}

std::string HostConfig::base_url() const {
  if (!public_url.empty()) return public_url;
  std::string addr = listen_address == "0.0.0.0" || listen_address == "::" ? "127.0.0.1" : listen_address;
  if (addr.find(':') != std::string::npos) addr = "[" + addr + "]";
  return "http://" + addr + ":" + std::to_string(listen_port);
}

std::shared_ptr<const HostConfig> local_mode(std::shared_ptr<const HostConfig> config) {
  auto out = std::make_shared<HostConfig>(*config);
  out->local = true;
  out->listen_address = "127.0.0.1";
  out->identity.cookie_secure = false;
  out->identity.mailer = "dev";
  out->public_url.clear();
  return out;
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

bool is_email(std::string_view s) {
  auto at = s.find('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= s.size() || s.size() > 254) return false;
  for (unsigned char c : s) {
    if (c <= 0x20 || c == '<' || c == '>' || c == '"' || c == ',' || c >= 0x7f) return false;
  }
  return s.find('@', at + 1) == std::string_view::npos;
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
  if (!check_keys(root, {"listen", "storage", "platform-domain", "public-url", "domains", "operations", "identity",
                          "threads", "limits"},
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
      if (!check_keys(entry, {"tenant", "owner"}, "domains." + domain, &err)) return *error = err, nullptr;
      auto tenant = get_string(entry, "tenant", &err);
      if (!tenant || !base::is_valid_name(*tenant)) {
        return *error = "domains." + domain + ".tenant must match [a-z][a-z0-9_]*", nullptr;
      }
      std::string owner = base::to_lower(get_string(entry, "owner", &err).value_or(""));
      if (!owner.empty() && !is_email(owner)) return *error = "domains." + domain + ".owner must be an email", nullptr;
      cfg->domains[domain] = DomainEntry{*tenant, owner};
    }
  }
  if (const auto* ops = get_object(root, "operations", &err)) {
    if (!check_keys(*ops, {"token"}, "operations", &err)) return *error = err, nullptr;
    if (auto v = get_string(*ops, "token", &err)) cfg->operations_token = *v;
  }
  if (!cfg->operations_token.empty() && cfg->operations_token.size() < 16) {
    *error = "operations.token must be at least 16 characters, or left out";
    return nullptr;
  }
  if (auto v = get_string(root, "public-url", &err)) {
    while (!v->empty() && v->back() == '/') v->pop_back();
    if (!v->starts_with("http://") && !v->starts_with("https://")) {
      return *error = "public-url must start with http:// or https://", nullptr;
    }
    cfg->public_url = *v;
  }
  if (const auto* id = get_object(root, "identity", &err)) {
    if (!check_keys(*id, {"cookie-secure", "session-days", "mailer", "operators", "clients"}, "identity", &err)) {
      return *error = err, nullptr;
    }
    if (auto v = get_bool(*id, "cookie-secure", &err)) cfg->identity.cookie_secure = *v;
    if (auto v = get_int(*id, "session-days", &err)) cfg->identity.session_days = *v;
    if (cfg->identity.session_days < 1 || cfg->identity.session_days > 400) {
      return *error = "identity.session-days must be 1 to 400", nullptr;
    }
    if (auto v = get_string(*id, "mailer", &err)) cfg->identity.mailer = *v;
    if (cfg->identity.mailer != "dev") return *error = "identity.mailer must be \"dev\" in v1", nullptr;
    if (const auto* ops = get_object(*id, "operators", &err)) {
      for (const auto& kv : *ops) {
        std::string email = base::to_lower(kv.key());
        if (!is_email(email)) return *error = "identity.operators: \"" + email + "\" is not an email", nullptr;
        if (!kv.value().is_array()) return *error = "identity.operators." + email + " must be a list of host roles", nullptr;
        auto& roles = cfg->identity.operators[email];
        for (const auto& r : kv.value().get_array()) {
          std::string_view role = r.is_string() ? std::string_view(r.get_string()) : "";
          std::string_view rest = role.starts_with("host:") ? role.substr(5) : "";
          if (!base::is_valid_name(rest)) {
            return *error = "identity.operators." + email + ": each role must look like host:owner", nullptr;
          }
          roles.emplace_back(role);
        }
      }
    }
    if (const auto* clients = get_object(*id, "clients", &err)) {
      for (const auto& kv : *clients) {
        std::string where = "identity.clients." + std::string(kv.key());
        if (!kv.value().is_object()) return *error = where + " must be an object", nullptr;
        const auto& c = kv.value().get_object();
        if (!check_keys(c, {"name", "redirect-uris", "acts-as"}, where, &err)) return *error = err, nullptr;
        IdentityConfig::Client client;
        auto name = get_string(c, "name", &err);
        if (!name || name->empty()) return *error = where + ".name is required", nullptr;
        client.name = *name;
        const auto* uris = c.if_contains("redirect-uris");
        if (!uris || !uris->is_array() || uris->get_array().empty()) {
          return *error = where + ".redirect-uris must be a list of URIs", nullptr;
        }
        for (const auto& u : uris->get_array()) {
          if (!u.is_string()) return *error = where + ".redirect-uris must be a list of URIs", nullptr;
          client.redirect_uris.emplace_back(u.get_string());
        }
        auto acts = get_string(c, "acts-as", &err);
        if (!acts || (*acts != "person" && *acts != "agent")) {
          return *error = where + ".acts-as must be \"person\" or \"agent\"", nullptr;
        }
        client.person = *acts == "person";
        cfg->identity.clients[std::string(kv.key())] = std::move(client);
      }
    }
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
