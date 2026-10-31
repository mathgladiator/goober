#include "keys.h"

#include <fcntl.h>
#include <cerrno>
#include <unistd.h>

#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/base/random.h"
#include "goober/base/encoding.h"
#include "goober/base/time.h"

namespace goober::host {
namespace fs = std::filesystem;
namespace bj = boost::json;

std::optional<fs::path> KeyStore::file(std::string_view scope, std::string_view tenant, std::string_view name) const {
  if (!base::is_valid_name(name)) return std::nullopt;
  if (scope == "system") return root_ / "system" / std::string(name);
  if (scope == "tenant" && base::is_valid_name(tenant)) return root_ / "tenant" / std::string(tenant) / std::string(name);
  return std::nullopt;
}

namespace {

// A host a key may go to: a name like api.example.com, *.example.com, or 127.0.0.1.
bool valid_key_host(const std::string& h) {
  std::string bare = h.starts_with("*.") ? h.substr(2) : h;
  if (bare.empty() || bare.size() > 253 || bare.find('.') == std::string::npos) return false;
  for (char c : bare) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
  }
  return true;
}

// The host of an http(s) URL, lowercased, without a port.
std::string host_of(std::string_view url) {
  auto scheme = url.find("://");
  if (scheme == std::string_view::npos) return "";
  url.remove_prefix(scheme + 3);
  auto end = url.find_first_of("/:?#");
  return base::to_lower(url.substr(0, end));
}

}  // namespace

bool KeyStore::set(std::string_view scope, std::string_view tenant, std::string_view name, std::string_view value,
                   int64_t now_ms, std::string* error, const std::vector<std::string>& hosts) {
  auto f = file(scope, tenant, name);
  if (!f) return *error = "a key is scope system, or tenant with a tenant, and a name like stripe_live", false;
  if (value.empty() || value.size() > 8192) return *error = "a key's value is 1 to 8192 bytes", false;
  for (unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) return *error = "a key's value can't hold control characters", false;
  }
  std::error_code mkdir_error;
  fs::create_directories(f->parent_path(), mkdir_error);
  bj::array host_list;
  for (const auto& h : hosts) {
    std::string lower = base::to_lower(h);
    if (!valid_key_host(lower)) return *error = "\"" + h + "\" isn't a host name like api.example.com or *.example.com", false;
    host_list.push_back(bj::value(lower));
  }
  std::string body = json::dump(bj::object{{"value", value}, {"set_at", now_ms}, {"hosts", host_list}});
  // Written 0600 from the start, then renamed into place.
  // A temporary name of its own, so two writers never interleave in one file.
  uint8_t rnd[8];
  base::os_random_fill(rnd);
  auto tmp = f->string() + "." + base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd)) + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return *error = "can't write the key", false;
  bool ok = ::write(fd, body.data(), body.size()) == ssize_t(body.size()) && ::fsync(fd) == 0;
  ::close(fd);
  std::error_code ec;
  if (ok) fs::rename(tmp, *f, ec);
  if (!ok || ec) {
    fs::remove(tmp, ec);
    return *error = "can't write the key", false;
  }
  // The rename is durable only once the directory is.
  int dir = ::open(f->parent_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (dir >= 0) {
    ::fsync(dir);
    ::close(dir);
  }
  return true;
}

std::optional<std::string> KeyStore::ensure_random_system_key(std::string_view name, int64_t now_ms, bool* created,
                                                                std::string* error) {
  *created = false;
  std::string credentials = "keys/system/" + std::string(name);
  if (auto existing = resolve(credentials, "")) return existing;
  auto f = file("system", "", name);
  if (!f) return *error = "a system key's name is like vault_key", std::nullopt;
  uint8_t key[32];
  base::os_random_fill(key);
  std::string hex = base::hex_encode(std::string_view(reinterpret_cast<char*>(key), sizeof key));
  std::error_code ec;
  fs::create_directories(f->parent_path(), ec);
  std::string body = json::dump(bj::object{{"value", hex}, {"set_at", now_ms}, {"hosts", bj::array{}}});
  uint8_t rnd[8];
  base::os_random_fill(rnd);
  auto tmp = f->string() + "." + base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd)) + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return *error = "can't write the key", std::nullopt;
  bool ok = ::write(fd, body.data(), body.size()) == ssize_t(body.size()) && ::fsync(fd) == 0;
  ::close(fd);
  // link() fails when the name exists: whoever links first made the key, and the other reads it.
  bool linked = ok && ::link(tmp.c_str(), f->c_str()) == 0;
  int link_errno = errno;
  fs::remove(tmp, ec);
  if (!linked) {
    if (ok && link_errno == EEXIST) {
      if (auto existing = resolve(credentials, "")) return existing;
    }
    return *error = "can't write the key", std::nullopt;
  }
  int dir = ::open(f->parent_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (dir >= 0) {
    ::fsync(dir);
    ::close(dir);
  }
  *created = true;
  return hex;
}

bool KeyStore::remove(std::string_view scope, std::string_view tenant, std::string_view name) {
  auto f = file(scope, tenant, name);
  return f && fs::exists(*f) && base::remove_file_durable(*f);
}

std::optional<std::string> KeyStore::resolve(std::string_view credentials, std::string_view tenant) const {
  std::optional<fs::path> f;
  if (credentials.starts_with("keys/system/")) f = file("system", "", credentials.substr(12));
  if (credentials.starts_with("keys/tenant/{!tenant}/")) f = file("tenant", tenant, credentials.substr(22));
  if (!f) return std::nullopt;
  auto text = base::read_file(*f);
  if (!text) return std::nullopt;
  std::string err;
  auto v = json::parse(*text, &err);
  const bj::value* value = v && v->is_object() ? v->as_object().if_contains("value") : nullptr;
  if (!value || !value->is_string()) return std::nullopt;
  return std::string(value->as_string());
}

std::optional<std::string> KeyStore::resolve_for(std::string_view credentials, std::string_view tenant, std::string_view url,
                                                 std::string* why) const {
  std::optional<fs::path> f;
  if (credentials.starts_with("keys/system/")) f = file("system", "", credentials.substr(12));
  if (credentials.starts_with("keys/tenant/{!tenant}/")) f = file("tenant", tenant, credentials.substr(22));
  auto text = f ? base::read_file(*f) : std::nullopt;
  std::string err;
  auto v = text ? json::parse(*text, &err) : std::nullopt;
  const bj::value* value = v && v->is_object() ? v->as_object().if_contains("value") : nullptr;
  if (!value || !value->is_string()) {
    *why = "the key " + std::string(credentials) + " isn't set; an operator sets it with key_set";
    return std::nullopt;
  }
  std::string host = host_of(url);
  const bj::value* hosts = v->as_object().if_contains("hosts");
  bool allowed = false;
  if (hosts && hosts->is_array()) {
    for (const auto& h : hosts->as_array()) {
      std::string allowed_host = h.is_string() ? std::string(h.as_string()) : "";
      allowed |= allowed_host == host || (allowed_host.starts_with("*.") && host.ends_with(allowed_host.substr(1)));
    }
  }
  if (!allowed) {
    *why = "the key " + std::string(credentials) + " may not be sent to " + host +
           ": an operator names the hosts a key goes to (key_set hosts)";
    return std::nullopt;
  }
  return std::string(value->as_string());
}

std::optional<aws::Credentials> KeyStore::aws_credentials() const {
  auto id = resolve("keys/system/aws_access_key_id", "");
  auto secret = resolve("keys/system/aws_secret_access_key", "");
  if (!id || !secret) return std::nullopt;
  return aws::Credentials{*id, *secret, resolve("keys/system/aws_session_token", "").value_or("")};
}

bj::array KeyStore::list() const {
  bj::array out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root_, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file() || it->path().extension() == ".tmp") continue;
    auto rel = fs::relative(it->path(), root_).generic_string();
    auto text = base::read_file(it->path());
    std::string err;
    auto v = text ? json::parse(*text, &err) : std::nullopt;
    if (!v || !v->is_object()) continue;
    const auto& o = v->as_object();
    std::string value = o.contains("value") ? std::string(o.at("value").as_string()) : "";
    // A fingerprint tells two values apart without saying anything about either.
    out.push_back(bj::object{{"key", "keys/" + rel},
                             {"set_at", o.contains("set_at") ? base::format_utc_ms(o.at("set_at").as_int64()) : ""},
                             {"hosts", o.contains("hosts") ? o.at("hosts") : bj::value(bj::array{})},
                             {"fingerprint", base::sha256_hex("goober-key-fingerprint\n" + value).substr(0, 12)}});
  }
  return out;
}

}  // namespace goober::host
