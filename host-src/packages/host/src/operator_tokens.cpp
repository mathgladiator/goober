#include "goober/host/operator_tokens.h"

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"
#include "goober/json/json.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPrefix = "gop_";

fs::path tokens_dir(const fs::path& storage) { return storage / "operations" / "tokens"; }

std::optional<OperatorToken> read_token(const fs::path& file) {
  auto text = base::read_file(file);
  if (!text) return std::nullopt;
  std::string err;
  auto v = json::parse(*text, &err);
  if (!v || !v->is_object()) return std::nullopt;
  const auto& o = v->get_object();
  OperatorToken t;
  t.id = file.stem().string().substr(0, 12);
  t.name = json::get_string(o, "name", &err).value_or("");
  t.created_ms = json::get_int(o, "created_ms", &err).value_or(0);
  if (const auto* roles = json::get_array(o, "roles", &err)) {
    for (const auto& r : *roles) {
      if (r.is_string()) t.roles.emplace_back(r.get_string());
    }
  }
  return t;
}

bool is_hex(std::string_view s) {
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

bool is_valid_token_name(std::string_view name) {
  if (name.empty() || name.size() > 63) return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

std::optional<std::string> create_operator_token(const fs::path& storage, std::string_view name, int64_t now_ms,
                                                 std::string* error) {
  if (!is_valid_token_name(name)) return *error = "a token name is [a-z0-9_-], 1 to 63 characters", std::nullopt;
  uint8_t raw[32];
  base::os_random_fill(raw);
  std::string token = std::string(kPrefix) + base::hex_encode(std::string_view(reinterpret_cast<char*>(raw), 32));
  fs::path file = tokens_dir(storage) / (base::sha256_hex(token) + ".json");
  bj::object body{{"name", name}, {"created_ms", now_ms}, {"roles", bj::array{"host:owner"}}};
  if (!base::write_file_durable(file, json::pretty(body))) {
    return *error = "cannot write " + file.string(), std::nullopt;
  }
  std::error_code ec;
  fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, ec);
  return token;
}

std::vector<OperatorToken> list_operator_tokens(const fs::path& storage) {
  std::vector<OperatorToken> out;
  for (const auto& rel : base::list_files(tokens_dir(storage))) {
    if (auto t = read_token(tokens_dir(storage) / rel)) out.push_back(std::move(*t));
  }
  return out;
}

int revoke_operator_tokens(const fs::path& storage, std::string_view which) {
  int n = 0;
  for (const auto& rel : base::list_files(tokens_dir(storage))) {
    auto file = tokens_dir(storage) / rel;
    auto t = read_token(file);
    if (t && (t->name == which || t->id == which) && base::remove_file_durable(file)) ++n;
  }
  return n;
}

std::optional<OperatorToken> find_operator_token(const fs::path& storage, std::string_view raw) {
  if (!raw.starts_with(kPrefix) || raw.size() != kPrefix.size() + 64 || !is_hex(raw.substr(kPrefix.size()))) {
    return std::nullopt;
  }
  return read_token(tokens_dir(storage) / (base::sha256_hex(raw) + ".json"));
}

}  // namespace goober::host
