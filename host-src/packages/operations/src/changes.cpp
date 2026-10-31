#include "goober/operations/changes.h"

#include <map>
#include <set>

#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/sqlite/sqlite.h"

namespace goober::operations {

namespace fs = std::filesystem;

namespace {

constexpr int64_t kTextDaysMs = 90LL * 24 * 3600 * 1000;
// Old files past this size are measured, not read (attachments can be large).
constexpr uint64_t kReadMaxBytes = 1 << 20;

std::vector<std::string> split(std::string_view path) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= path.size()) {
    size_t end = path.find('/', i);
    if (end == std::string_view::npos) end = path.size();
    out.emplace_back(path.substr(i, end - i));
    i = end + 1;
  }
  return out;
}

bool is_text(std::string_view s) { return s.size() <= ChangeLog::kTextMaxBytes && s.find('\0') == std::string_view::npos; }

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

PathKind classify_path(std::string_view path) {
  auto parts = split(path);
  PathKind k;
  if (parts.size() >= 3 && parts[0] == "content") {
    k.tenant = parts[1];
    k.target = std::string(path.substr(parts[0].size() + parts[1].size() + 2));
    // partial_put's kept copies: the partial's own row has the before and after.
    if (k.target.starts_with("_partials/_versions/")) return PathKind{};
    k.kind = k.target.starts_with("_partials/") ? "partial" : k.target.starts_with("_templates/") ? "template" : "content";
    return k;
  }
  if (parts.size() >= 3 && parts[0] == "boxes") {
    k.kind = "box";
    k.target = parts[1];
    if (parts.size() == 3 && parts[2] == "active") {
      k.active = true;
    } else if (parts.size() >= 4 && parts[2] == "versions") {
      k.version = parts[3];
    }
    return k;
  }
  if (parts.size() == 2 && parts[0] == "schemas" && parts[1].ends_with(".schema.json")) {
    k.kind = "schema";
    k.target = parts[1].substr(0, parts[1].size() - std::string_view(".schema.json").size());
    return k;
  }
  if (parts.size() == 4 && parts[0] == "objects" && parts[2] == "attachments") {
    k.kind = "attachment";
    k.tenant = parts[1];
    k.target = parts[3];
    return k;
  }
  k.kind = "other";
  k.target = std::string(path);
  return k;
}

ChangeLog::ChangeLog(fs::path db_file, fs::path storage_root) : root_(std::move(storage_root)) {
  fs::create_directories(db_file.parent_path());
  db_ = std::make_unique<sqlite::Db>(db_file);
  db_->exec(
      "CREATE TABLE IF NOT EXISTS changes (id INTEGER PRIMARY KEY AUTOINCREMENT, at_ms INTEGER NOT NULL,"
      " kind TEXT NOT NULL, action TEXT NOT NULL, tenant TEXT NOT NULL, target TEXT NOT NULL, actor TEXT NOT NULL,"
      " request_id TEXT NOT NULL, bytes_before INTEGER, bytes_after INTEGER, hash_before TEXT, hash_after TEXT,"
      " before BLOB, after BLOB, detail TEXT NOT NULL);"
      "CREATE INDEX IF NOT EXISTS changes_request ON changes (request_id);"
      "CREATE TABLE IF NOT EXISTS actions (id INTEGER PRIMARY KEY AUTOINCREMENT, at_ms INTEGER NOT NULL,"
      " operator TEXT NOT NULL, tool TEXT NOT NULL, request_id TEXT NOT NULL, args TEXT NOT NULL,"
      " failed INTEGER NOT NULL);");
}

ChangeLog::~ChangeLog() = default;

std::optional<std::string> ChangeLog::read_old(std::string_view path, uint64_t* size) const {
  std::error_code ec;
  fs::path file = root_ / std::string(path);
  auto s = fs::file_size(file, ec);
  if (ec) return std::nullopt;
  *size = s;
  if (s > kReadMaxBytes) return std::nullopt;
  return base::read_file(file);
}

void ChangeLog::insert(int64_t now_ms, const std::string& kind, const std::string& action, const std::string& tenant,
                       const std::string& target, const std::string& actor, const std::string& request_id,
                       const std::optional<std::string>& before, const std::optional<std::string>& after,
                       bool keep_text, const bj::object& detail) {
  sqlite::Stmt s(*db_,
                 "INSERT INTO changes (at_ms, kind, action, tenant, target, actor, request_id, bytes_before, "
                 "bytes_after, hash_before, hash_after, before, after, detail) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                 "?, ?, ?, ?)");
  s.bind(1, now_ms).bind(2, kind).bind(3, action).bind(4, tenant).bind(5, target).bind(6, actor).bind(7, request_id);
  auto side = [&](int col, const std::optional<std::string>& v, int64_t size) {
    if (size >= 0) {
      s.bind(col, size);
    } else {
      s.bind_null(col);
    }
    if (v) {
      s.bind(col + 2, base::sha256_hex(*v));
    } else {
      s.bind_null(col + 2);
    }
    if (v && keep_text && is_text(*v) && is_text(before.value_or("")) && is_text(after.value_or(""))) {
      s.bind_blob(col + 4, *v);
    } else {
      s.bind_null(col + 4);
    }
  };
  auto size_of = [&](const char* k, const std::optional<std::string>& v) -> int64_t {
    if (auto* d = detail.if_contains(k); d && d->is_int64()) return d->as_int64();
    return v ? int64_t(v->size()) : -1;
  };
  side(8, before, size_of("_bytes_before", before));
  side(9, after, size_of("_bytes_after", after));
  bj::object shown;
  for (const auto& kv : detail) {
    if (!kv.key().starts_with("_")) shown[kv.key()] = kv.value();
  }
  s.bind(14, json::dump(shown));
  s.run();
}

void ChangeLog::record(const wal::Bundle& bundle, const std::string& actor, const std::string& request_id,
                       int64_t now_ms) {
  std::lock_guard lock(mu_);
  // A box's files in one bundle are one deploy (or upload) row.
  std::map<std::string, std::pair<std::optional<std::string>, std::string>> boxes;  // box -> (new active, uploaded version)
  std::set<std::string> disabled;  // boxes whose active version was taken away (box_disable)
  db_->exec("BEGIN IMMEDIATE");
  try {
    for (const auto& change : bundle) {
      auto k = classify_path(change.path);
      if (k.kind.empty()) continue;
      bool del = change.kind == wal::FileChange::Kind::kDelete;
      if (k.kind == "box") {
        auto& b = boxes[k.target];
        if (k.active && !del) b.first = trim(change.data);
        if (k.active && del) disabled.insert(k.target);
        if (!k.version.empty()) b.second = k.version;
        continue;
      }
      uint64_t old_size = 0;
      auto before = read_old(change.path, &old_size);
      bool existed = before || old_size > 0;
      bj::object detail;
      if (!before && existed) detail["_bytes_before"] = int64_t(old_size);
      std::optional<std::string> after;
      if (!del) {
        if (change.data.size() <= kReadMaxBytes) {
          after = change.data;
        } else {
          detail["_bytes_after"] = int64_t(change.data.size());
        }
      }
      if (!existed) {
        std::error_code ec;
        existed = fs::exists(root_ / change.path, ec);
      }
      bool texts = k.kind == "content" || k.kind == "partial" || k.kind == "template" || k.kind == "schema";
      std::string action = del ? "delete" : existed ? "write" : "create";
      insert(now_ms, k.kind, action, k.tenant, k.target, actor, request_id, before, after, texts, detail);
    }
    for (const auto& [box, b] : boxes) {
      bj::object detail;
      if (b.first) {
        uint64_t size = 0;
        auto previous = read_old("boxes/" + box + "/active", &size);
        detail["version"] = *b.first;
        if (previous) detail["previous"] = trim(*previous);
        detail["uploaded"] = !b.second.empty();
        insert(now_ms, "deploy", "activate", "", box, actor, request_id,
               previous ? std::optional<std::string>(trim(*previous)) : std::nullopt, b.first, false, detail);
      } else if (!b.second.empty()) {
        detail["version"] = b.second;
        insert(now_ms, "upload", "write", "", box, actor, request_id, std::nullopt, std::nullopt, false, detail);
      }
    }
    for (const auto& box : disabled) {
      uint64_t size = 0;
      auto previous = read_old("boxes/" + box + "/active", &size);
      bj::object detail;
      if (previous) detail["version"] = trim(*previous);
      insert(now_ms, "deploy", "disable", "", box, actor, request_id,
             previous ? std::optional<std::string>(trim(*previous)) : std::nullopt, std::nullopt, false, detail);
    }
    if (now_ms - last_trim_ms_ > 3600 * 1000) {
      last_trim_ms_ = now_ms;
      sqlite::Stmt t(*db_, "UPDATE changes SET before = NULL, after = NULL WHERE at_ms < ? AND (before IS NOT NULL OR after IS NOT NULL)");
      t.bind(1, now_ms - kTextDaysMs);
      t.run();
    }
    db_->exec("COMMIT");
  } catch (...) {
    db_->exec("ROLLBACK");
    throw;
  }
}

void ChangeLog::record_action(const Action& a) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_,
                 "INSERT INTO actions (at_ms, operator, tool, request_id, args, failed) VALUES (?, ?, ?, ?, ?, ?)");
  s.bind(1, a.at_ms).bind(2, a.operator_name).bind(3, a.tool).bind(4, a.request_id).bind(5, json::dump(a.args));
  s.bind(6, int64_t(a.failed ? 1 : 0));
  s.run();
}

namespace {

bj::object row_of(sqlite::Stmt& s, bool with_text) {
  // id, at_ms, kind, action, tenant, target, actor, request_id, bytes_before, bytes_after,
  // hash_before, hash_after, has_text, detail[, before, after]
  auto opt_int = [&](int c) { return s.is_null(c) ? bj::value(nullptr) : bj::value(s.int64(c)); };
  auto opt_text = [&](int c) { return s.is_null(c) ? bj::value(nullptr) : bj::value(s.text(c)); };
  std::string err;
  auto detail = json::parse(s.text(13), &err);
  bj::object o{{"id", s.int64(0)},
               {"at_ms", s.int64(1)},
               {"kind", s.text(2)},
               {"action", s.text(3)},
               {"tenant", s.text(4)},
               {"target", s.text(5)},
               {"actor", s.text(6)},
               {"request_id", s.text(7)},
               {"bytes_before", opt_int(8)},
               {"bytes_after", opt_int(9)},
               {"hash_before", opt_text(10)},
               {"hash_after", opt_text(11)},
               {"has_text", s.int64(12) != 0},
               {"detail", detail && detail->is_object() ? *detail : bj::value(bj::object{})}};
  if (with_text) {
    o["before"] = s.is_null(14) ? bj::value(nullptr) : bj::value(s.blob(14));
    o["after"] = s.is_null(15) ? bj::value(nullptr) : bj::value(s.blob(15));
  }
  return o;
}

constexpr const char* kColumns =
    "id, at_ms, kind, action, tenant, target, actor, request_id, bytes_before, bytes_after, hash_before, hash_after, "
    "(before IS NOT NULL OR after IS NOT NULL), detail";

}  // namespace

bj::array ChangeLog::list(const std::string& kind, const std::string& tenant, int64_t before_id, int64_t limit) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, std::string("SELECT ") + kColumns +
                           " FROM changes WHERE (? = '' OR kind = ?) AND (? = '' OR tenant = ?) AND (? <= 0 OR id < ?)"
                           " ORDER BY id DESC LIMIT ?");
  s.bind(1, kind).bind(2, kind).bind(3, tenant).bind(4, tenant).bind(5, before_id).bind(6, before_id).bind(7, limit);
  bj::array out;
  while (s.step()) out.push_back(row_of(s, false));
  return out;
}

std::optional<bj::object> ChangeLog::get(int64_t id) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, std::string("SELECT ") + kColumns + ", before, after FROM changes WHERE id = ?");
  s.bind(1, id);
  if (!s.step()) return std::nullopt;
  return row_of(s, true);
}

int64_t ChangeLog::last_id() {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, "SELECT coalesce(max(id), 0) FROM changes");
  return s.step() ? s.int64(0) : 0;
}

std::vector<int64_t> ChangeLog::ids_since(const std::string& request_id, int64_t after_id) {
  std::lock_guard lock(mu_);
  std::vector<int64_t> out;
  if (request_id.empty()) return out;
  sqlite::Stmt s(*db_, "SELECT id FROM changes WHERE request_id = ? AND id > ? ORDER BY id");
  s.bind(1, request_id).bind(2, after_id);
  while (s.step()) out.push_back(s.int64(0));
  return out;
}

bj::array ChangeLog::actions(bool changed_only, int64_t before_id, int64_t limit) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_,
                 "SELECT id, at_ms, operator, tool, request_id, args, failed FROM actions a WHERE (? <= 0 OR id < ?) AND"
                 " (? = 0 OR (a.request_id != '' AND EXISTS (SELECT 1 FROM changes c WHERE c.request_id = a.request_id)))"
                 " ORDER BY id DESC LIMIT ?");
  s.bind(1, before_id).bind(2, before_id).bind(3, int64_t(changed_only ? 1 : 0)).bind(4, limit);
  bj::array out;
  while (s.step()) {
    std::string err;
    auto args = json::parse(s.text(5), &err);
    bj::array ids;
    std::string rid = s.text(4);
    if (!rid.empty()) {
      sqlite::Stmt c(*db_, "SELECT id FROM changes WHERE request_id = ? ORDER BY id");
      c.bind(1, rid);
      while (c.step()) ids.push_back(c.int64(0));
    }
    out.push_back(bj::object{{"id", s.int64(0)},
                             {"at_ms", s.int64(1)},
                             {"operator", s.text(2)},
                             {"tool", s.text(3)},
                             {"request_id", rid},
                             {"args", args && args->is_object() ? *args : bj::value(bj::object{})},
                             {"failed", s.int64(6) != 0},
                             {"changes", std::move(ids)}});
  }
  return out;
}

}  // namespace goober::operations
