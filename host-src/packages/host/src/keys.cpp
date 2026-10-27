#include "keys.h"

#include <fcntl.h>
#include <unistd.h>

#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
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

bool KeyStore::set(std::string_view scope, std::string_view tenant, std::string_view name, std::string_view value,
                   int64_t now_ms, std::string* error) {
  auto f = file(scope, tenant, name);
  if (!f) return *error = "a key is scope system, or tenant with a tenant, and a name like stripe_live", false;
  if (value.empty() || value.size() > 8192) return *error = "a key's value is 1 to 8192 bytes", false;
  for (unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) return *error = "a key's value can't hold control characters", false;
  }
  fs::create_directories(f->parent_path());
  std::string body = json::dump(bj::object{{"value", value}, {"set_at", now_ms}});
  // Written 0600 from the start, then renamed into place.
  auto tmp = f->string() + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return *error = "can't write the key", false;
  bool ok = ::write(fd, body.data(), body.size()) == ssize_t(body.size()) && ::fsync(fd) == 0;
  ::close(fd);
  std::error_code ec;
  if (ok) fs::rename(tmp, *f, ec);
  if (!ok || ec) return *error = "can't write the key", false;
  return true;
}

bool KeyStore::remove(std::string_view scope, std::string_view tenant, std::string_view name) {
  auto f = file(scope, tenant, name);
  std::error_code ec;
  return f && fs::remove(*f, ec);
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
  if (!v || !v->is_object() || !v->as_object().contains("value")) return std::nullopt;
  return std::string(v->as_object().at("value").as_string());
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
                             {"fingerprint", base::sha256_hex("goober-key-fingerprint\n" + value).substr(0, 12)}});
  }
  return out;
}

}  // namespace goober::host
