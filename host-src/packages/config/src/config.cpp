#include "goober/config/config.h"

#include <cctype>
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

namespace {

// A Host header value without its port or trailing dot, lowercased.
std::string host_name(std::string_view host) {
  std::string h = base::to_lower(host);
  if (!h.empty() && h.front() == '[') {
    auto close = h.find(']');
    if (close != std::string::npos) h = h.substr(0, close + 1);
  } else if (auto colon = h.rfind(':'); colon != std::string::npos) {
    h = h.substr(0, colon);
  }
  if (!h.empty() && h.back() == '.') h.pop_back();
  return h;
}

}  // namespace

std::optional<std::pair<std::string, DomainEntry>> HostConfig::resolve_host(std::string_view host) const {
  std::string h = host_name(host);
  if (auto it = domains.find(h); it != domains.end()) return *it;
  if (!local || domains.empty()) return std::nullopt;
  constexpr std::string_view kSuffix = ".localhost";
  if (h.size() > kSuffix.size() && h.ends_with(kSuffix)) {
    if (auto it = domains.find(h.substr(0, h.size() - kSuffix.size())); it != domains.end()) return *it;
    return std::nullopt;
  }
  if (h == "localhost" || h == "127.0.0.1" || h == "[::1]" || h == "10.0.2.2") {  // 10.0.2.2: the Android emulator's name for this machine
    if (auto it = domains.find(platform_domain); it != domains.end()) return *it;
    return *domains.begin();
  }
  return std::nullopt;
}

std::optional<std::string> HostConfig::redirect_for(std::string_view host) const {
  std::string h = host_name(host);
  if (auto it = redirects.find(h); it != redirects.end()) return it->second;
  constexpr std::string_view kSuffix = ".localhost";
  if (local && h.size() > kSuffix.size() && h.ends_with(kSuffix)) {
    if (auto it = redirects.find(h.substr(0, h.size() - kSuffix.size())); it != redirects.end()) {
      return it->second + std::string(kSuffix);
    }
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

// Like ops.example.com: labels of letters, digits, and hyphens, at least two of them.
bool is_dns_name(std::string_view s) {
  if (s.size() > 253 || s.find('.') == std::string_view::npos) return false;
  size_t label = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    char c = i < s.size() ? s[i] : '.';
    if (c == '.') {
      if (label == 0 || label > 63 || s[i - 1] == '-' || s[i - label] == '-') return false;
      label = 0;
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      ++label;
    } else {
      return false;
    }
  }
  return true;
}

// Like us-east-1 or eu-central-2.
bool is_region(std::string_view s) {
  if (s.empty() || s.size() > 32) return false;
  for (char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

// "" (the service's own address), https://host[:port], or plain http on this machine.
bool is_endpoint(std::string_view s) {
  if (s.empty()) return true;
  std::string_view rest = s.starts_with("https://") ? s.substr(8)
                          : s.starts_with("http://127.0.0.1:") || s.starts_with("http://localhost:") ? s.substr(7)
                                                                                                     : std::string_view();
  if (rest.empty()) return false;
  while (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
  for (char c : rest) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':')) {
      return false;
    }
  }
  return !rest.empty();
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
                          "threads", "limits", "web", "sandbox", "aws", "mail", "backup", "source", "pwa", "tenants", "audit", "cache"},
                  "config", &err)) {
    *error = err;
    return nullptr;
  }

  if (const auto* listen = get_object(root, "listen", &err)) {
    if (!check_keys(*listen, {"address", "port", "behind-proxy"}, "listen", &err)) return *error = err, nullptr;
    if (auto v = get_string(*listen, "address", &err)) cfg->listen_address = *v;
    if (auto v = get_bool(*listen, "behind-proxy", &err)) cfg->behind_proxy = *v;
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
      if (!check_keys(entry, {"tenant", "owner", "redirect-to"}, "domains." + domain, &err)) return *error = err, nullptr;
      if (entry.contains("redirect-to")) {
        // A redirect is only that: no tenant, no owner (015.Domains.md).
        if (entry.size() != 1) return *error = "domains." + domain + " has \"redirect-to\", so it takes nothing else", nullptr;
        auto to = get_string(entry, "redirect-to", &err);
        if (!to) return *error = "domains." + domain + ".redirect-to must be a domain, like example.com", nullptr;
        cfg->redirects[domain] = base::to_lower(*to);
        continue;
      }
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
    if (!check_keys(*ops, {"token", "domain", "listen"}, "operations", &err)) return *error = err, nullptr;
    if (auto v = get_string(*ops, "token", &err)) cfg->operations_token = *v;
    if (auto v = get_string(*ops, "domain", &err)) {
      std::string d = base::to_lower(*v);
      if (!is_dns_name(d)) return *error = "operations.domain must be a DNS name, like ops.example.com", nullptr;
      cfg->operations_domain = d;
    }
    if (auto v = get_string(*ops, "listen", &err)) {
      // address:port, the address in brackets when it is IPv6.
      auto colon = v->rfind(':');
      std::string address = colon == std::string::npos ? "" : v->substr(0, colon);
      if (address.size() > 2 && address.front() == '[' && address.back() == ']') address = address.substr(1, address.size() - 2);
      int64_t port = -1;
      if (colon != std::string::npos && colon + 1 < v->size() && v->size() - colon <= 6) {
        port = 0;
        for (char c : v->substr(colon + 1)) port = c >= '0' && c <= '9' && port >= 0 ? port * 10 + (c - '0') : -1;
      }
      if (address.empty() || port < 0 || port > 65535) {
        return *error = "operations.listen must be address:port, like 127.0.0.1:9443", nullptr;
      }
      cfg->operations_listen_address = address;
      cfg->operations_listen_port = uint16_t(port);
    }
  }
  for (const auto& [from, to] : cfg->redirects) {
    if (!is_dns_name(from)) return *error = "domains." + from + ": a redirect domain must be a DNS name, like www.example.com", nullptr;
    if (cfg->domains.count(from)) return *error = "domains." + from + " is listed twice, once as a redirect", nullptr;
    if (!cfg->domains.count(to)) {
      return *error = "domains." + from + ".redirect-to is " + to + ", which isn't a tenant's domain in \"domains\"", nullptr;
    }
  }
  if (cfg->redirects.count(cfg->operations_domain)) {
    *error = "operations.domain " + cfg->operations_domain + " is also a redirect domain: the panel needs a domain of its own";
    return nullptr;
  }
  if (cfg->domains.count(cfg->operations_domain)) {
    *error = "operations.domain " + cfg->operations_domain +
             " is also a tenant's domain: the panel needs a domain of its own, like ops." + cfg->operations_domain;
    return nullptr;
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
    if (!check_keys(*id, {"cookie-secure", "session-days", "mailer", "smtp", "ses", "operators", "clients"}, "identity", &err)) {
      return *error = err, nullptr;
    }
    if (auto v = get_bool(*id, "cookie-secure", &err)) cfg->identity.cookie_secure = *v;
    if (auto v = get_int(*id, "session-days", &err)) cfg->identity.session_days = *v;
    if (cfg->identity.session_days < 1 || cfg->identity.session_days > 400) {
      return *error = "identity.session-days must be 1 to 400", nullptr;
    }
    if (auto v = get_string(*id, "mailer", &err)) cfg->identity.mailer = *v;
    if (cfg->identity.mailer != "dev" && cfg->identity.mailer != "smtp" && cfg->identity.mailer != "ses") {
      return *error = "identity.mailer must be \"dev\", \"smtp\", or \"ses\"", nullptr;
    }
    if (const auto* smtp = get_object(*id, "smtp", &err)) {
      if (!check_keys(*smtp, {"url", "from", "username", "password-file"}, "identity.smtp", &err)) {
        return *error = err, nullptr;
      }
      auto& m = cfg->identity.smtp;
      m.url = get_string(*smtp, "url", &err).value_or("");
      m.from = base::to_lower(get_string(*smtp, "from", &err).value_or(""));
      m.username = get_string(*smtp, "username", &err).value_or("");
      m.password_file = get_string(*smtp, "password-file", &err).value_or("");
      std::string_view rest = m.url.starts_with("smtps://") ? std::string_view(m.url).substr(8)
                              : m.url.starts_with("smtp://") ? std::string_view(m.url).substr(7)
                                                             : std::string_view();
      bool host_ok = !rest.empty() && rest.size() < 256;
      for (char c : rest) {
        host_ok = host_ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                              c == '-' || c == ':' || c == '[' || c == ']');
      }
      if (!host_ok) return *error = "identity.smtp.url must be smtps://host[:port] or smtp://host[:port]", nullptr;
      if (!is_email(m.from)) return *error = "identity.smtp.from must be an email address", nullptr;
      for (char c : m.username) {
        if (static_cast<unsigned char>(c) < 0x20) return *error = "identity.smtp.username holds a control character", nullptr;
      }
      if (!m.username.empty() && m.password_file.empty()) {
        return *error = "identity.smtp.password-file is required with a username", nullptr;
      }
    }
    if (const auto* ses = get_object(*id, "ses", &err)) {
      if (!check_keys(*ses, {"from", "region", "configuration-set", "endpoint"}, "identity.ses", &err)) {
        return *error = err, nullptr;
      }
      auto& s = cfg->identity.ses;
      s.from = base::to_lower(get_string(*ses, "from", &err).value_or(""));
      s.region = get_string(*ses, "region", &err).value_or("");
      s.configuration_set = get_string(*ses, "configuration-set", &err).value_or("");
      s.endpoint = get_string(*ses, "endpoint", &err).value_or("");
      if (!is_email(s.from)) return *error = "identity.ses.from must be an email address", nullptr;
      if (!s.region.empty() && !is_region(s.region)) return *error = "identity.ses.region isn't an AWS region", nullptr;
      if (!is_endpoint(s.endpoint)) return *error = "identity.ses.endpoint must be https://host or http://127.0.0.1:port", nullptr;
    }
    if (cfg->identity.mailer == "ses" && cfg->identity.ses.from.empty()) {
      return *error = "identity.mailer \"ses\" needs identity.ses {from}", nullptr;
    }
    if (cfg->identity.mailer == "smtp" && cfg->identity.smtp.url.empty()) {
      return *error = "identity.mailer \"smtp\" needs identity.smtp {url, from, username, password-file}", nullptr;
    }
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
    if (!check_keys(*limits, {"body-max-bytes", "timeout-ms", "fuel", "memory-max-bytes", "requests-per-minute",
                              "outbox-daily-per-service"},
                    "limits", &err)) {
      return *error = err, nullptr;
    }
    if (auto v = get_int(*limits, "body-max-bytes", &err)) cfg->body_max_bytes = *v;
    if (auto v = get_int(*limits, "outbox-daily-per-service", &err)) {
      if (*v < 1 || *v > 10'000'000) return *error = "limits.outbox-daily-per-service must be 1 to 10000000", nullptr;
      cfg->outbox_daily_per_service = *v;
    }
    if (auto v = get_int(*limits, "requests-per-minute", &err)) {
      if (*v < 10 || *v > 1'000'000) return *error = "limits.requests-per-minute must be 10 to 1000000", nullptr;
      cfg->requests_per_minute = *v;
    }
    if (!read_limit(*limits, "timeout-ms", cfg->timeout_ms, &err) || !read_limit(*limits, "fuel", cfg->fuel, &err) ||
        !read_limit(*limits, "memory-max-bytes", cfg->memory_max_bytes, &err)) {
      return *error = err, nullptr;
    }
  }
  if (const auto* web = get_object(root, "web", &err)) {
    if (!check_keys(*web, {"contact", "min-delay-ms", "max-page-bytes", "opt-out"}, "web", &err)) return *error = err, nullptr;
    if (auto v = get_string(*web, "contact", &err)) cfg->web.contact = *v;
    for (char c : cfg->web.contact) {
      if (static_cast<unsigned char>(c) < 0x20 || c == '(' || c == ')') return *error = "web.contact holds a character a User-Agent can't", nullptr;
    }
    if (auto v = get_int(*web, "min-delay-ms", &err)) {
      if (*v < 250 || *v > 600000) return *error = "web.min-delay-ms must be 250 to 600000", nullptr;
      cfg->web.min_delay_ms = *v;
    }
    if (auto v = get_int(*web, "max-page-bytes", &err)) {
      if (*v < 4096 || *v > (64 << 20)) return *error = "web.max-page-bytes must be 4096 to 67108864", nullptr;
      cfg->web.max_page_bytes = *v;
    }
    if (const auto* list = get_array(*web, "opt-out", &err)) {
      for (const auto& h : *list) {
        if (!h.is_string()) return *error = "web.opt-out holds host names", nullptr;
        cfg->web.opt_out.emplace_back(base::to_lower(h.as_string()));
      }
    }
  }
  if (auto v = get_bool(root, "sandbox", &err)) cfg->sandbox = *v;
  if (const auto* aws = get_object(root, "aws", &err)) {
    if (!check_keys(*aws, {"region"}, "aws", &err)) return *error = err, nullptr;
    if (auto v = get_string(*aws, "region", &err)) cfg->aws.region = *v;
    if (!is_region(cfg->aws.region)) return *error = "aws.region isn't an AWS region", nullptr;
  }
  if (const auto* mail = get_object(root, "mail", &err)) {
    if (!check_keys(*mail, {"daily-max", "per-address-daily", "reply-per-address-daily", "outreach-daily-max", "inbound"}, "mail", &err)) return *error = err, nullptr;
    if (const auto* in = get_object(*mail, "inbound", &err)) {
      if (!check_keys(*in, {"token-file", "max-bytes", "attachment-days"}, "mail.inbound", &err)) return *error = err, nullptr;
      auto& inbound = cfg->mail.inbound;
      if (auto v = get_string(*in, "token-file", &err)) {
        inbound.token_file = *v;
        if (inbound.token_file.is_relative()) inbound.token_file = base_dir / inbound.token_file;
      }
      if (auto v = get_int(*in, "max-bytes", &err)) inbound.max_bytes = *v;
      if (auto v = get_int(*in, "attachment-days", &err)) inbound.attachment_days = *v;
      if (inbound.max_bytes < 1024 || inbound.max_bytes > (25LL << 20) || inbound.attachment_days < 1 ||
          inbound.attachment_days > 3650) {
        return *error = "mail.inbound.max-bytes is 1024 to 26214400 (25 MiB), and attachment-days 1 to 3650", nullptr;
      }
    }
    if (auto v = get_int(*mail, "daily-max", &err)) cfg->mail.daily_max = *v;
    if (auto v = get_int(*mail, "per-address-daily", &err)) cfg->mail.per_address_daily = *v;
    if (auto v = get_int(*mail, "reply-per-address-daily", &err)) cfg->mail.reply_per_address_daily = *v;
    if (auto v = get_int(*mail, "outreach-daily-max", &err)) cfg->mail.outreach_daily_max = *v;
    if (cfg->mail.outreach_daily_max < 0 || cfg->mail.outreach_daily_max > 10000) {
      return *error = "mail.outreach-daily-max is 0 to 10000", nullptr;
    }
    if (cfg->mail.reply_per_address_daily < 1 || cfg->mail.reply_per_address_daily > 1000) {
      return *error = "mail.reply-per-address-daily is 1 to 1000", nullptr;
    }
    if (cfg->mail.daily_max < 0 || cfg->mail.daily_max > 1000000 || cfg->mail.per_address_daily < 1 ||
        cfg->mail.per_address_daily > 1000) {
      return *error = "mail.daily-max is 0 to 1000000 and mail.per-address-daily 1 to 1000", nullptr;
    }
  }
  if (const auto* backup = get_object(root, "backup", &err)) {
    if (!check_keys(*backup, {"s3", "nightly-at", "keep-days", "databases-every-ms"}, "backup", &err)) return *error = err, nullptr;
    auto& b = cfg->backup;
    if (const auto* s3 = get_object(*backup, "s3", &err)) {
      if (!check_keys(*s3, {"bucket", "region", "endpoint", "prefix"}, "backup.s3", &err)) return *error = err, nullptr;
      b.bucket = get_string(*s3, "bucket", &err).value_or("");
      b.region = get_string(*s3, "region", &err).value_or("");
      b.endpoint = get_string(*s3, "endpoint", &err).value_or("");
      b.prefix = get_string(*s3, "prefix", &err).value_or("");
      bool bucket_ok = b.bucket.size() >= 3 && b.bucket.size() <= 63;
      for (char c : b.bucket) bucket_ok = bucket_ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.');
      if (!bucket_ok) return *error = "backup.s3.bucket must be an S3 bucket name", nullptr;
      if (!b.region.empty() && !is_region(b.region)) return *error = "backup.s3.region isn't an AWS region", nullptr;
      if (!is_endpoint(b.endpoint)) return *error = "backup.s3.endpoint must be https://host or http://127.0.0.1:port", nullptr;
      if (!b.prefix.empty() && (!base::is_safe_relative_path(b.prefix.substr(0, b.prefix.size() - (b.prefix.back() == '/'))))) {
        return *error = "backup.s3.prefix must be a relative path like goober/", nullptr;
      }
      if (!b.prefix.empty() && b.prefix.back() != '/') b.prefix += '/';
    } else if (err.empty()) {
      return *error = "backup needs s3 {bucket}", nullptr;
    }
    if (auto v = get_string(*backup, "nightly-at", &err)) {
      bool ok = v->size() == 5 && (*v)[2] == ':' && std::isdigit((unsigned char)(*v)[0]) && std::isdigit((unsigned char)(*v)[1]) &&
                std::isdigit((unsigned char)(*v)[3]) && std::isdigit((unsigned char)(*v)[4]);
      int h = ok ? std::stoi(v->substr(0, 2)) : -1, m = ok ? std::stoi(v->substr(3, 2)) : -1;
      if (!ok || h > 23 || m > 59) return *error = "backup.nightly-at must be HH:MM, in UTC", nullptr;
      b.nightly_minute = h * 60 + m;
    }
    if (auto v = get_int(*backup, "keep-days", &err)) {
      if (*v < 1 || *v > 365) return *error = "backup.keep-days must be 1 to 365", nullptr;
      b.keep_days = int(*v);
    }
    if (auto v = get_int(*backup, "databases-every-ms", &err)) {
      if (*v < 60000 || *v > 86400000) return *error = "backup.databases-every-ms must be 60000 to 86400000", nullptr;
      b.databases_every_ms = *v;
    }
  }
  if (const auto* pwa = get_object(root, "pwa", &err)) {
    if (!check_keys(*pwa, {"push-hosts", "push-contact"}, "pwa", &err)) return *error = err, nullptr;
    if (const auto* hosts = get_array(*pwa, "push-hosts", &err)) {
      for (const auto& h : *hosts) {
        std::string host = h.is_string() ? base::to_lower(h.as_string()) : "";
        std::string bare = host.starts_with("*.") ? host.substr(2) : host;
        if (!is_dns_name(bare) && bare != "127.0.0.1" && bare != "localhost") {
          return *error = "pwa.push-hosts holds host names, like push.example.com or *.push.example.com", nullptr;
        }
        cfg->pwa.push_hosts.push_back(host);
      }
    }
    if (auto v = get_string(*pwa, "push-contact", &err)) {
      if (!v->starts_with("mailto:") && !v->starts_with("https://")) {
        return *error = "pwa.push-contact is mailto:you@example.com or an https URL", nullptr;
      }
      cfg->pwa.push_contact = *v;
    }
  }
  // What `goober-host audit` (a deploy's operations audit) stops suggesting: a check's id, or
  // a check's id and the one name it stops suggesting, like "redirect:www.example.com".
  if (const auto* cache = get_object(root, "cache", &err)) {
    if (!check_keys(*cache, {"bytes"}, "cache", &err)) return *error = err, nullptr;
    if (auto v = get_int(*cache, "bytes", &err)) {
      if (*v < 0 || *v > (int64_t(64) << 30)) return *error = "cache.bytes is 0 (off) to 68719476736 (64 GiB)", nullptr;
      cfg->cache_bytes = *v;
    }
  }
  if (const auto* audit = get_object(root, "audit", &err)) {
    if (!check_keys(*audit, {"quiet"}, "audit", &err)) return *error = err, nullptr;
    if (const auto* quiet = get_array(*audit, "quiet", &err)) {
      for (const auto& q : *quiet) {
        std::string id = q.is_string() ? std::string(q.as_string()) : "";
        auto colon = id.find(':');
        std::string check = id.substr(0, colon);
        bool ok = !check.empty() && check.size() <= 40;
        for (char ch : check) ok = ok && ((ch >= 'a' && ch <= 'z') || ch == '-');
        if (colon != std::string::npos) ok = ok && is_dns_name(id.substr(colon + 1));
        if (!ok) return *error = "audit.quiet holds check ids, like \"panel\" or \"redirect:www.example.com\"", nullptr;
        cfg->audit_quiet.push_back(id);
      }
    }
  }
  if (const auto* tenants = get_object(root, "tenants", &err)) {
    for (const auto& kv : *tenants) {
      std::string name(kv.key()), where = "tenants." + name;
      bool served = false;
      for (const auto& [d, e] : cfg->domains) served |= e.tenant == name;
      if (!served) return *error = where + ": no domain serves tenant " + name, nullptr;
      if (!kv.value().is_object()) return *error = where + " is an object", nullptr;
      const auto& t = kv.value().as_object();
      if (!check_keys(t, {"postal-address", "terms", "privacy", "invites"}, where.c_str(), &err)) return *error = err, nullptr;
      TenantConfig tc;
      tc.postal_address = get_string(t, "postal-address", &err).value_or("");
      if (tc.postal_address.size() > 200 || tc.postal_address.find_first_of("\r\n") != std::string::npos) {
        return *error = where + ".postal-address is one line of at most 200 characters", nullptr;
      }
      for (const char* k : {"terms", "privacy"}) {
        std::string v = get_string(t, k, &err).value_or("");
        if (!v.empty() && (v[0] != '/' || v.size() > 200 || v.find_first_of(" ?#\\") != std::string::npos)) {
          return *error = where + "." + k + " is a path on the site, like /terms", nullptr;
        }
        (std::string(k) == "terms" ? tc.terms : tc.privacy) = v;
      }
      if (const auto* inv = get_object(t, "invites", &err)) {
        std::string iw = where + ".invites";
        if (!check_keys(*inv, {"roles", "daily-max", "per-inviter-daily"}, iw.c_str(), &err)) return *error = err, nullptr;
        if (const auto* roles = get_array(*inv, "roles", &err)) {
          for (const auto& r : *roles) {
            std::string role = r.is_string() ? std::string(r.as_string()) : "";
            bool ok = !role.empty() && role.find(':') == std::string::npos && role.size() <= 64;
            for (char ch : role) ok = ok && (std::isalnum((unsigned char)ch) || ch == '_' || ch == '/');
            if (!ok) return *error = iw + ".roles holds tenant roles, like member or editor/blog", nullptr;
            tc.invites.roles.push_back(role);
          }
        }
        if (auto v = get_int(*inv, "daily-max", &err)) tc.invites.daily_max = *v;
        if (auto v = get_int(*inv, "per-inviter-daily", &err)) tc.invites.per_inviter_daily = *v;
        if (tc.invites.daily_max < 0 || tc.invites.daily_max > 10000 || tc.invites.per_inviter_daily < 1 ||
            tc.invites.per_inviter_daily > 1000) {
          return *error = iw + ".daily-max is 0 to 10000 and per-inviter-daily 1 to 1000", nullptr;
        }
      }
      cfg->tenants[name] = std::move(tc);
    }
  }
  if (const auto* source = get_object(root, "source", &err)) {
    if (!check_keys(*source, {"cargo", "vendor", "sandbox", "build-timeout-ms"}, "source", &err)) return *error = err, nullptr;
    auto& s = cfg->source;
    if (auto v = get_string(*source, "cargo", &err)) s.cargo = *v;
    if (auto v = get_string(*source, "vendor", &err)) s.vendor = (base_dir / *v).lexically_normal().string();
    if (auto v = get_string(*source, "sandbox", &err)) s.sandbox = *v;
    if (s.sandbox != "bwrap" && s.sandbox != "none") return *error = "source.sandbox must be \"bwrap\" or \"none\"", nullptr;
    if (auto v = get_int(*source, "build-timeout-ms", &err)) {
      if (*v < 10000 || *v > 3600000) return *error = "source.build-timeout-ms must be 10000 to 3600000", nullptr;
      s.build_timeout_ms = *v;
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
