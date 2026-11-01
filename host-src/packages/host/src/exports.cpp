// Sealed CSV exports (design/machine/008.Databases.md § 7.1).

#include "exports.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>

#include "goober/backup/backup.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/json/json.h"

namespace goober::host::exports {
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

bool valid_id(const std::string& id) {
  return id.size() == 16 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The tenant in the context too: an export moved under another tenant's name fails its check.
std::string context(const std::string& tenant, const std::string& id) { return "goober-export\n" + tenant + "\n" + id; }

std::string cell(const bj::value& v) {
  if (v.is_null()) return "";
  if (v.is_string()) return std::string(v.get_string());
  if (v.is_bool()) return v.get_bool() ? "1" : "0";
  if (const auto* o = v.if_object()) {
    if (const auto* b = o->if_contains("base64"); b && b->is_string()) return std::string(b->get_string());
  }
  return json::dump(v);
}

std::string quoted(const std::string& s) {
  if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

std::optional<Meta> read_meta(const fs::path& file) {
  auto text = base::read_file(file);
  if (!text) return std::nullopt;
  std::string err;
  auto v = json::parse(*text, &err);
  if (!v || !v->is_object()) return std::nullopt;
  const auto& o = v->as_object();
  auto str = [&](const char* k) {
    const auto* x = o.if_contains(k);
    return x && x->is_string() ? std::string(x->get_string()) : std::string();
  };
  auto num = [&](const char* k) {
    const auto* x = o.if_contains(k);
    return x && x->is_int64() ? x->get_int64() : int64_t(0);
  };
  Meta m;
  m.id = str("export");
  m.tenant = str("tenant");
  m.database = str("database");
  m.sql = str("sql");
  m.operator_name = str("operator");
  m.sha256 = str("sha256");
  m.rows = num("rows");
  m.bytes = num("bytes");
  m.created_ms = num("created_ms");
  m.expires_ms = num("expires_ms");
  if (const auto* c = o.if_contains("columns"); c && c->is_array()) {
    for (const auto& x : c->as_array()) m.columns.push_back(x.is_string() ? std::string(x.get_string()) : json::dump(x));
  }
  if (!valid_id(m.id)) return std::nullopt;
  return m;
}

}  // namespace

bj::object Meta::to_json() const {
  bj::array cols;
  for (const auto& c : columns) cols.push_back(bj::string(c));
  return bj::object{{"export", id},
                    {"tenant", tenant},
                    {"database", database},
                    {"sql", sql},
                    {"operator", operator_name},
                    {"columns", std::move(cols)},
                    {"rows", rows},
                    {"bytes", bytes},
                    {"sha256", sha256},
                    {"created_ms", created_ms},
                    {"created", base::format_utc_ms(created_ms)},
                    {"expires_ms", expires_ms},
                    {"expires", base::format_utc_ms(expires_ms)}};
}

fs::path dir(const fs::path& storage) { return storage / "exports"; }

std::string to_csv(const bj::object& result) {
  std::string out;
  auto line = [&](const bj::array& cells) {
    for (size_t i = 0; i < cells.size(); ++i) {
      if (i) out += ',';
      out += quoted(cell(cells[i]));
    }
    out += "\r\n";
  };
  if (const auto* c = result.if_contains("columns"); c && c->is_array()) line(c->as_array());
  if (const auto* r = result.if_contains("rows"); r && r->is_array()) {
    for (const auto& row : r->as_array()) {
      if (row.is_array()) line(row.as_array());
    }
  }
  return out;
}

std::optional<Meta> write(const fs::path& storage, const vault::Key& vault_key, const std::string& tenant,
                          const std::string& database, const std::string& sql, const std::string& operator_name,
                          const bj::object& result, int64_t now_ms, std::string* error) {
  list(storage, now_ms);  // the expired go first
  std::string csv = to_csv(result);
  Meta m;
  char id[17];
  std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(base::os_random_u64()));
  m.id = id;
  m.tenant = tenant;
  m.database = database;
  m.sql = sql;
  m.operator_name = operator_name;
  m.sha256 = base::sha256_hex(csv);
  m.bytes = int64_t(csv.size());
  m.created_ms = now_ms;
  m.expires_ms = now_ms + kKeepMs;
  if (const auto* c = result.if_contains("columns"); c && c->is_array()) {
    for (const auto& x : c->as_array()) m.columns.push_back(cell(x));
  }
  if (const auto* r = result.if_contains("rows"); r && r->is_array()) m.rows = int64_t(r->as_array().size());
  fs::path d = dir(storage);
  std::error_code ec;
  fs::create_directories(d, ec);
  fs::permissions(d, fs::perms::owner_all, fs::perm_options::replace, ec);
  if (!backup::seal_bytes(vault::tenant_key(vault_key, tenant), csv, d / (m.id + ".csv.sealed"), error, context(tenant, m.id))) {
    return std::nullopt;
  }
  if (!base::write_file_durable(d / (m.id + ".json"), json::pretty(m.to_json()))) {
    fs::remove(d / (m.id + ".csv.sealed"), ec);
    return *error = "can't write " + (d / (m.id + ".json")).string(), std::nullopt;
  }
  return m;
}

std::vector<Meta> list(const fs::path& storage, int64_t now_ms) {
  std::vector<Meta> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir(storage), ec)) {
    if (e.path().extension() != ".json") continue;
    auto m = read_meta(e.path());
    if (!m) continue;
    if (m->expires_ms <= now_ms) {
      remove(storage, m->id);
      continue;
    }
    out.push_back(std::move(*m));
  }
  std::sort(out.begin(), out.end(), [](const Meta& a, const Meta& b) { return a.created_ms > b.created_ms; });
  return out;
}

std::optional<Meta> find(const fs::path& storage, const std::string& id, int64_t now_ms) {
  if (!valid_id(id)) return std::nullopt;
  auto m = read_meta(dir(storage) / (id + ".json"));
  if (!m) return std::nullopt;
  if (m->expires_ms <= now_ms) {
    remove(storage, id);
    return std::nullopt;
  }
  return m;
}

bool open_to(const fs::path& storage, const vault::Key& vault_key, const std::string& id, const fs::path& out, int64_t now_ms,
             std::string* error) {
  auto m = find(storage, id, now_ms);
  if (!m) return *error = "no export " + id + " (exports are kept 7 days; `goober-host export list` shows them)", false;
  std::error_code ec;
  // "-" is standard output, so a person can fetch it over ssh into a file on their own machine
  // (unsealed into tmp, owner-only, and removed at once).
  bool to_stdout = out == "-";
  fs::path target = to_stdout ? storage / "tmp" / ("export-" + id + ".csv") : out;
  if (to_stdout) {
    fs::create_directories(target.parent_path(), ec);
    fs::remove(target, ec);
  } else if (fs::exists(out, ec)) {
    return *error = out.string() + " exists already; name another file with --out", false;
  }
  mode_t old = ::umask(077);
  bool ok = backup::unseal_file(vault::tenant_key(vault_key, m->tenant), dir(storage) / (id + ".csv.sealed"), target, error,
                                context(m->tenant, id));
  ::umask(old);
  if (!ok) {
    *error = "export " + id + " doesn't open: the vault key changed, or the file was altered (" + *error + ")";
    return false;
  }
  fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
  if (to_stdout) {
    auto text = base::read_file(target);
    fs::remove(target, ec);
    if (!text) return *error = "can't read the unsealed export back", false;
    std::fwrite(text->data(), 1, text->size(), stdout);
    std::fflush(stdout);
  }
  return true;
}

bool remove(const fs::path& storage, const std::string& id) {
  if (!valid_id(id)) return false;
  std::error_code ec;
  bool a = fs::remove(dir(storage) / (id + ".csv.sealed"), ec);
  bool b = fs::remove(dir(storage) / (id + ".json"), ec);
  return a || b;
}

}  // namespace goober::host::exports
