#include "goober/attachments/attachments.h"

#include <cctype>
#include <filesystem>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/names.h"
#include "goober/sqlite/sqlite.h"

namespace goober::attachments {
namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS uploads (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  folder TEXT NOT NULL,
  state TEXT NOT NULL,
  host TEXT NOT NULL,
  host_epoch INTEGER NOT NULL,
  content_type TEXT,
  ext TEXT,
  size INTEGER,
  sha256 TEXT,
  name TEXT NOT NULL DEFAULT '',
  alt TEXT NOT NULL DEFAULT '',
  visibility TEXT NOT NULL DEFAULT 'public',
  created_at INTEGER NOT NULL,
  stored_at INTEGER,
  committed_at INTEGER,
  orphaned_at INTEGER
);
)sql";

constexpr const char* kColumns =
    "id, state, content_type, ext, size, sha256, name, alt, visibility, created_at";

std::unique_ptr<sqlite::Db> open(const wal::HostWal& wal, std::string_view tenant) {
  auto dir = wal.root() / "sqlite3" / std::string(tenant);
  std::filesystem::create_directories(dir);
  auto db = std::make_unique<sqlite::Db>(dir / "internal.db");
  db->exec(kSchema);
  return db;
}

Attachment row(const sqlite::Stmt& q) {
  Attachment a;
  a.id = q.int64(0);
  a.state = q.text(1);
  a.content_type = q.text(2);
  a.ext = q.text(3);
  a.size = q.int64(4);
  a.sha256 = q.text(5);
  a.name = q.text(6);
  a.alt = q.text(7);
  a.visibility = q.text(8);
  a.created_ms = q.int64(9);
  return a;
}

std::string bytes_path(std::string_view tenant, int64_t id) {
  return "objects/" + std::string(tenant) + "/attachments/" + std::to_string(id);
}

bool starts(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

}  // namespace

bool Attachment::inline_ok() const { return ext != "svg" && ext != "pdf"; }

std::optional<Sniffed> sniff(std::string_view b) {
  if (starts(b, "\x89PNG\r\n\x1a\n")) return Sniffed{"image/png", "png", true};
  if (starts(b, "\xFF\xD8\xFF")) return Sniffed{"image/jpeg", "jpg", true};
  if (starts(b, "GIF87a") || starts(b, "GIF89a")) return Sniffed{"image/gif", "gif", true};
  if (b.size() >= 12 && starts(b, "RIFF") && b.substr(8, 4) == "WEBP") return Sniffed{"image/webp", "webp", true};
  if (b.size() >= 12 && b.substr(4, 4) == "ftyp" && (b.substr(8, 4) == "avif" || b.substr(8, 4) == "avis")) {
    return Sniffed{"image/avif", "avif", true};
  }
  if (b.size() >= 6 && starts(b, std::string_view("\0\0\1\0", 4))) return Sniffed{"image/x-icon", "ico", true};
  if (starts(b, "%PDF-")) return Sniffed{"application/pdf", "pdf", false};
  std::string_view text = b.substr(0, 2048);
  if (starts(text, "\xEF\xBB\xBF")) text.remove_prefix(3);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  std::string lower = base::to_lower(text);
  if (starts(lower, "<svg") || ((starts(lower, "<?xml") || starts(lower, "<!--")) && lower.find("<svg") != std::string::npos)) {
    return Sniffed{"image/svg+xml", "svg", false};
  }
  return std::nullopt;
}

std::optional<Attachment> Store::put(std::string_view tenant, std::string_view bytes, std::string_view name,
                                     std::string_view alt, std::string_view visibility, int64_t now_ms,
                                     std::string* error) {
  if (!base::is_valid_name(tenant)) return *error = "tenant must match [a-z][a-z0-9_]*", std::nullopt;
  if (visibility.empty()) visibility = "public";
  if (visibility != "public" && visibility != "members") {
    return *error = "visibility must be public or members", std::nullopt;
  }
  if (bytes.empty()) return *error = "the attachment is empty", std::nullopt;
  auto type = sniff(bytes);
  if (!type) return *error = "not an image or PDF: attachments must be PNG, JPEG, GIF, WebP, AVIF, ICO, SVG, or PDF", std::nullopt;
  std::string clean_name(name.substr(0, 200));
  for (auto& c : clean_name) {
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  }
  auto db = open(wal_, tenant);
  sqlite::Stmt ins(*db,
                   "INSERT INTO uploads (folder, state, host, host_epoch, content_type, ext, size, sha256, name, alt, "
                   "visibility, created_at) VALUES ('attachments', 'writing', 'local', 0, ?, ?, ?, ?, ?, ?, ?, ?)");
  ins.bind(1, type->content_type).bind(2, type->ext).bind(3, int64_t(bytes.size())).bind(4, base::sha256_hex(bytes));
  ins.bind(5, clean_name).bind(6, alt.substr(0, 500)).bind(7, visibility).bind(8, now_ms);
  ins.run();
  int64_t id = db->last_insert_rowid();
  std::string err;
  bool ok = wal_.commit({{wal::FileChange::Kind::kWrite, bytes_path(tenant, id), std::string(bytes)}}, &err);
  sqlite::Stmt done(*db, "UPDATE uploads SET state = ?, stored_at = ? WHERE id = ?");
  done.bind(1, std::string_view(ok ? "stored" : "failed")).bind(2, now_ms).bind(3, id).run();
  if (!ok) return *error = "storing the bytes failed: " + err, std::nullopt;
  return get(tenant, id);
}

std::optional<Attachment> Store::get(std::string_view tenant, int64_t id) const {
  if (!base::is_valid_name(tenant)) return std::nullopt;
  if (!std::filesystem::exists(wal_.root() / "sqlite3" / std::string(tenant) / "internal.db")) return std::nullopt;
  auto db = open(wal_, tenant);
  sqlite::Stmt q(*db, std::string("SELECT ") + kColumns + " FROM uploads WHERE id = ? AND state = 'stored'");
  q.bind(1, id);
  if (!q.step()) return std::nullopt;
  return row(q);
}

std::optional<std::string> Store::read(std::string_view tenant, int64_t id) const {
  if (!base::is_valid_name(tenant)) return std::nullopt;
  return base::read_file(wal_.root() / bytes_path(tenant, id));
}

std::vector<Attachment> Store::list(std::string_view tenant) const {
  std::vector<Attachment> out;
  if (!base::is_valid_name(tenant)) return out;
  if (!std::filesystem::exists(wal_.root() / "sqlite3" / std::string(tenant) / "internal.db")) return out;
  auto db = open(wal_, tenant);
  sqlite::Stmt q(*db, std::string("SELECT ") + kColumns + " FROM uploads WHERE state = 'stored' ORDER BY id DESC");
  while (q.step()) out.push_back(row(q));
  return out;
}

bool Store::remove(std::string_view tenant, int64_t id, int64_t now_ms, std::string* error) {
  if (!get(tenant, id)) return *error = "no attachment " + std::to_string(id), false;
  auto db = open(wal_, tenant);
  // The row stays, so the id is never minted again and the ledger says what happened.
  sqlite::Stmt mark(*db, "UPDATE uploads SET state = 'deleted', orphaned_at = ? WHERE id = ?");
  mark.bind(1, now_ms).bind(2, id).run();
  return wal_.commit({{wal::FileChange::Kind::kDelete, bytes_path(tenant, id), ""}}, error);
}

std::map<int64_t, std::vector<std::string>> references(const content::ContentStore& content, std::string_view tenant) {
  std::map<int64_t, std::vector<std::string>> out;
  static const char* kText[] = {".md", ".html", ".css", ".js", ".json", ".txt", ".xml", ".svg"};
  for (const auto& path : content.list(tenant, "")) {
    bool text = false;
    for (const char* ext : kText) text = text || path.ends_with(ext);
    if (!text) continue;
    auto file = content.get(tenant, path);
    if (!file) continue;
    std::string_view data = file->data;
    for (size_t at = data.find("/attachment/"); at != std::string_view::npos; at = data.find("/attachment/", at + 1)) {
      size_t i = at + 12;
      int64_t id = 0;
      size_t digits = 0;
      while (i < data.size() && std::isdigit(static_cast<unsigned char>(data[i])) && digits < 18) {
        id = id * 10 + (data[i] - '0');
        ++i;
        ++digits;
      }
      if (digits == 0) continue;
      auto& paths = out[id];
      if (paths.empty() || paths.back() != path) paths.push_back(path);
    }
  }
  return out;
}

}  // namespace goober::attachments
