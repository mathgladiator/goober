// Source projects in one SQLite file (design/machine/020.Source.md §2).

#include <algorithm>
#include <map>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/source/source.h"
#include "goober/sqlite/sqlite.h"

namespace goober::source {
namespace fs = std::filesystem;

namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS projects (
  name TEXT PRIMARY KEY, description TEXT NOT NULL, head INTEGER NOT NULL DEFAULT 0,
  created_ms INTEGER NOT NULL, updated_ms INTEGER NOT NULL,
  deployed_rev INTEGER NOT NULL DEFAULT 0, deployed_ms INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS revisions (
  project TEXT NOT NULL, rev INTEGER NOT NULL, message TEXT NOT NULL, author TEXT NOT NULL, at_ms INTEGER NOT NULL,
  PRIMARY KEY (project, rev)) WITHOUT ROWID;
-- Every revision's whole tree: a revision is never rebuilt from others.
CREATE TABLE IF NOT EXISTS trees (
  project TEXT NOT NULL, rev INTEGER NOT NULL, path TEXT NOT NULL, sha256 TEXT NOT NULL, size INTEGER NOT NULL,
  PRIMARY KEY (project, rev, path)) WITHOUT ROWID;
-- What each revision touched, for the log and for telling whether two commits overlap.
CREATE TABLE IF NOT EXISTS changes (
  project TEXT NOT NULL, rev INTEGER NOT NULL, path TEXT NOT NULL, PRIMARY KEY (project, rev, path)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS blobs (sha256 TEXT PRIMARY KEY, size INTEGER NOT NULL, data BLOB NOT NULL) WITHOUT ROWID;
)sql";

struct Txn {
  sqlite::Db& db;
  bool done = false;
  explicit Txn(sqlite::Db& d) : db(d) { db.exec("BEGIN IMMEDIATE"); }
  void commit() {
    db.exec("COMMIT");
    done = true;
  }
  ~Txn() {
    if (!done) {
      try {
        db.exec("ROLLBACK");
      } catch (...) {
      }
    }
  }
};

}  // namespace

bool valid_project_name(std::string_view name) {
  if (name.empty() || name.size() > 63 || !(name[0] >= 'a' && name[0] <= 'z')) return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return name.back() != '-';
}

bool valid_path(std::string_view path) {
  return !path.empty() && path.size() <= kMaxPathBytes && base::is_safe_relative_path(path) &&
         path.find_first_of(std::string_view("\0\r\n", 3)) == std::string_view::npos;
}

bool is_text(std::string_view bytes) {
  return bytes.find('\0') == std::string_view::npos && base::valid_utf8(bytes) == bytes;
}

Store::Store(fs::path file) {
  fs::create_directories(file.parent_path());
  db_ = std::make_unique<sqlite::Db>(file);
  db_->exec(kSchema);
}

Store::~Store() = default;

bool Store::create(const std::string& name, const std::string& description, int64_t now_ms, std::string* error) {
  if (!valid_project_name(name)) return *error = "a project name is like ranches or beef-share-site", false;
  if (description.size() > 2000) return *error = "a description is at most 2000 bytes", false;
  std::lock_guard lock(mu_);
  sqlite::Stmt count(*db_, "SELECT count(*) FROM projects");
  count.step();
  if (count.int64(0) >= int64_t(kMaxProjects)) return *error = "a host keeps at most " + std::to_string(kMaxProjects) + " projects", false;
  sqlite::Stmt s(*db_, "INSERT OR IGNORE INTO projects (name, description, created_ms, updated_ms) VALUES (?, ?, ?, ?)");
  s.bind(1, name).bind(2, description).bind(3, now_ms).bind(4, now_ms).run();
  if (db_->changes() == 0) return *error = "a project named " + name + " already exists", false;
  return true;
}

std::vector<Project> Store::list() {
  std::lock_guard lock(mu_);
  std::vector<Project> out;
  sqlite::Stmt s(*db_, "SELECT name, description, head, created_ms, updated_ms, deployed_rev, deployed_ms FROM projects ORDER BY name");
  while (s.step()) {
    out.push_back(Project{s.text(0), s.text(1), s.int64(2), s.int64(3), s.int64(4), s.int64(5), s.int64(6)});
  }
  return out;
}

std::optional<Project> Store::get(const std::string& name) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, "SELECT name, description, head, created_ms, updated_ms, deployed_rev, deployed_ms FROM projects WHERE name = ?");
  s.bind(1, name);
  if (!s.step()) return std::nullopt;
  return Project{s.text(0), s.text(1), s.int64(2), s.int64(3), s.int64(4), s.int64(5), s.int64(6)};
}

std::optional<int64_t> Store::resolve(const std::string& name, int64_t rev, std::string* error) {
  sqlite::Stmt s(*db_, "SELECT head FROM projects WHERE name = ?");
  s.bind(1, name);
  if (!s.step()) return *error = "no project named " + name, std::nullopt;
  int64_t head = s.int64(0);
  if (rev == 0) return head;
  if (rev < 0 || rev > head) return *error = name + " has revisions 1 to " + std::to_string(head), std::nullopt;
  return rev;
}

std::optional<std::vector<FileEntry>> Store::files(const std::string& name, int64_t rev, std::string* error) {
  std::lock_guard lock(mu_);
  auto r = resolve(name, rev, error);
  if (!r) return std::nullopt;
  std::vector<FileEntry> out;
  sqlite::Stmt s(*db_, "SELECT path, sha256, size FROM trees WHERE project = ? AND rev = ? ORDER BY path");
  s.bind(1, name).bind(2, *r);
  while (s.step()) out.push_back(FileEntry{s.text(0), s.text(1), s.int64(2)});
  return out;
}

std::optional<std::string> Store::read(const std::string& name, int64_t rev, const std::string& path, std::string* error) {
  std::lock_guard lock(mu_);
  auto r = resolve(name, rev, error);
  if (!r) return std::nullopt;
  sqlite::Stmt s(*db_, "SELECT b.data FROM trees t JOIN blobs b ON b.sha256 = t.sha256 "
                       "WHERE t.project = ? AND t.rev = ? AND t.path = ?");
  s.bind(1, name).bind(2, *r).bind(3, path);
  if (!s.step()) return *error = "no file " + path + " at revision " + std::to_string(*r), std::nullopt;
  return s.blob(0);
}

std::optional<CommitResult> Store::commit(const std::string& name, int64_t base, const std::vector<Change>& changes,
                                          const std::string& message, const std::string& author, int64_t now_ms,
                                          std::string* error, std::vector<std::string>* conflicts) {
  if (changes.empty()) return *error = "a commit changes at least one file", std::nullopt;
  if (message.empty() || message.size() > 4000) return *error = "a commit message is 1 to 4000 bytes", std::nullopt;
  std::map<std::string, const Change*> by_path;
  for (const auto& c : changes) {
    if (!valid_path(c.path)) return *error = "not a path a project can hold: " + c.path.substr(0, 100), std::nullopt;
    if (c.content && c.content->size() > kMaxFileBytes) {
      return *error = c.path + " is larger than " + std::to_string(kMaxFileBytes >> 20) + " MiB", std::nullopt;
    }
    if (!by_path.emplace(c.path, &c).second) return *error = c.path + " is changed twice in one commit", std::nullopt;
  }
  std::lock_guard lock(mu_);
  auto head = resolve(name, 0, error);
  if (!head) return std::nullopt;
  if (base < 0 || base > *head) return *error = "base must be a revision of " + name + " (0 to " + std::to_string(*head) + ")", std::nullopt;
  CommitResult result;
  // Others committed since base: fine, unless they touched the same files.
  if (base < *head) {
    std::set<std::string> theirs;
    sqlite::Stmt s(*db_, "SELECT DISTINCT path FROM changes WHERE project = ? AND rev > ?");
    s.bind(1, name).bind(2, base);
    while (s.step()) theirs.insert(s.text(0));
    std::vector<std::string> both;
    for (const auto& [path, _] : by_path) {
      if (theirs.count(path)) both.push_back(path);
    }
    if (!both.empty()) {
      if (conflicts) *conflicts = both;
      std::string list;
      for (const auto& p : both) list += (list.empty() ? "" : ", ") + p;
      return *error = "revision " + std::to_string(*head) + " is newer than " + std::to_string(base) +
                      " and changed the same files: " + list + ". Read them again and commit on top of " +
                      std::to_string(*head),
             std::nullopt;
    }
    result.rebased = true;
  }
  // The new tree: the head's, with the changes.
  std::map<std::string, std::pair<std::string, int64_t>> tree;
  {
    sqlite::Stmt s(*db_, "SELECT path, sha256, size FROM trees WHERE project = ? AND rev = ?");
    s.bind(1, name).bind(2, *head);
    while (s.step()) tree[s.text(0)] = {s.text(1), s.int64(2)};
  }
  std::vector<std::string> changed;
  for (const auto& [path, c] : by_path) {
    if (!c->content) {
      if (tree.erase(path)) changed.push_back(path);
      continue;
    }
    std::string sha = base::sha256_hex(*c->content);
    auto it = tree.find(path);
    if (it != tree.end() && it->second.first == sha) continue;  // the same bytes: no change
    // A file can't sit where a directory is, nor a directory where a file is.
    for (const auto& [other, _] : tree) {
      if (other.starts_with(path + "/") || path.starts_with(other + "/")) {
        if (!(by_path.count(other) && !by_path.at(other)->content)) {
          return *error = path + " and " + other + " can't both exist: one would be a directory", std::nullopt;
        }
      }
    }
    tree[path] = {sha, int64_t(c->content->size())};
    changed.push_back(path);
  }
  if (changed.empty()) return *error = "nothing changed: every file already holds those bytes", std::nullopt;
  if (tree.size() > kMaxFiles) return *error = "a project holds at most " + std::to_string(kMaxFiles) + " files", std::nullopt;
  size_t total = 0;
  for (const auto& [_, e] : tree) total += size_t(e.second);
  if (total > kMaxProjectBytes) return *error = "a project holds at most " + std::to_string(kMaxProjectBytes >> 20) + " MiB", std::nullopt;
  // History is kept forever, so it is bounded too: every distinct file the project has held.
  {
    sqlite::Stmt h(*db_, "SELECT coalesce(sum(size), 0) FROM blobs WHERE sha256 IN (SELECT DISTINCT sha256 FROM trees WHERE project = ?)");
    h.bind(1, name);
    h.step();
    size_t history = size_t(h.int64(0));
    for (const auto& [path, c] : by_path) {
      if (c->content) history += c->content->size();
    }
    if (history > kMaxHistoryBytes) {
      return *error = name + "'s history would pass " + std::to_string(kMaxHistoryBytes >> 30) +
                      " GiB: large files belong in attachments, not source",
             std::nullopt;
    }
  }

  Txn txn(*db_);
  int64_t rev = *head + 1;
  for (const auto& [path, c] : by_path) {
    if (!c->content) continue;
    sqlite::Stmt b(*db_, "INSERT OR IGNORE INTO blobs (sha256, size, data) VALUES (?, ?, ?)");
    b.bind(1, base::sha256_hex(*c->content)).bind(2, int64_t(c->content->size())).bind_blob(3, *c->content).run();
  }
  for (const auto& [path, e] : tree) {
    sqlite::Stmt t(*db_, "INSERT INTO trees (project, rev, path, sha256, size) VALUES (?, ?, ?, ?, ?)");
    t.bind(1, name).bind(2, rev).bind(3, path).bind(4, e.first).bind(5, e.second).run();
  }
  for (const auto& path : changed) {
    sqlite::Stmt c(*db_, "INSERT INTO changes (project, rev, path) VALUES (?, ?, ?)");
    c.bind(1, name).bind(2, rev).bind(3, path).run();
  }
  sqlite::Stmt r(*db_, "INSERT INTO revisions (project, rev, message, author, at_ms) VALUES (?, ?, ?, ?, ?)");
  r.bind(1, name).bind(2, rev).bind(3, message).bind(4, author).bind(5, now_ms).run();
  sqlite::Stmt p(*db_, "UPDATE projects SET head = ?, updated_ms = ? WHERE name = ?");
  p.bind(1, rev).bind(2, now_ms).bind(3, name).run();
  txn.commit();
  result.rev = rev;
  return result;
}

std::optional<int64_t> Store::revert(const std::string& name, int64_t rev, const std::string& author, int64_t now_ms,
                                     std::string* error) {
  std::vector<Change> changes;
  int64_t head = 0;
  {
    std::lock_guard lock(mu_);
    auto target = resolve(name, rev, error);
    if (!target) return std::nullopt;
    if (rev == 0) return *error = "say which revision to go back to", std::nullopt;
    head = *resolve(name, 0, error);
    std::map<std::string, std::string> then, now;
    sqlite::Stmt a(*db_, "SELECT path, sha256 FROM trees WHERE project = ? AND rev = ?");
    a.bind(1, name).bind(2, *target);
    while (a.step()) then[a.text(0)] = a.text(1);
    sqlite::Stmt b(*db_, "SELECT path, sha256 FROM trees WHERE project = ? AND rev = ?");
    b.bind(1, name).bind(2, head);
    while (b.step()) now[b.text(0)] = b.text(1);
    for (const auto& [path, sha] : now) {
      if (!then.count(path)) changes.push_back(Change{path, std::nullopt});
    }
    for (const auto& [path, sha] : then) {
      auto it = now.find(path);
      if (it != now.end() && it->second == sha) continue;
      sqlite::Stmt d(*db_, "SELECT data FROM blobs WHERE sha256 = ?");
      d.bind(1, sha);
      if (!d.step()) return *error = "a file of revision " + std::to_string(rev) + " is missing", std::nullopt;
      changes.push_back(Change{path, d.blob(0)});
    }
  }
  if (changes.empty()) return *error = "the head already holds revision " + std::to_string(rev) + "'s files", std::nullopt;
  auto r = commit(name, head, changes, "Back to revision " + std::to_string(rev), author, now_ms, error);
  if (!r) return std::nullopt;
  return r->rev;
}

std::vector<Revision> Store::log(const std::string& name, int limit) {
  std::lock_guard lock(mu_);
  std::vector<Revision> out;
  sqlite::Stmt s(*db_, "SELECT rev, message, author, at_ms FROM revisions WHERE project = ? ORDER BY rev DESC LIMIT ?");
  s.bind(1, name).bind(2, int64_t(std::clamp(limit, 1, 1000)));
  while (s.step()) out.push_back(Revision{s.int64(0), s.text(1), s.text(2), s.int64(3), {}});
  for (auto& r : out) {
    sqlite::Stmt c(*db_, "SELECT path FROM changes WHERE project = ? AND rev = ? ORDER BY path LIMIT 200");
    c.bind(1, name).bind(2, r.rev);
    while (c.step()) r.changed.push_back(c.text(0));
  }
  return out;
}

std::optional<std::string> Store::diff(const std::string& name, int64_t from, int64_t to,
                                       const std::vector<std::string>& paths, std::string* error) {
  std::map<std::string, std::string> a, b;
  int64_t ra = 0, rb = 0;
  {
    std::lock_guard lock(mu_);
    auto x = resolve(name, from, error);
    auto y = x ? resolve(name, to, error) : std::nullopt;
    if (!x || !y) return std::nullopt;
    ra = *x;
    rb = *y;
    auto load = [&](int64_t rev, std::map<std::string, std::string>* out) {
      sqlite::Stmt s(*db_, "SELECT path, sha256 FROM trees WHERE project = ? AND rev = ?");
      s.bind(1, name).bind(2, rev);
      while (s.step()) {
        if (paths.empty() || std::find(paths.begin(), paths.end(), s.text(0)) != paths.end()) (*out)[s.text(0)] = s.text(1);
      }
    };
    load(ra, &a);
    load(rb, &b);
  }
  std::set<std::string> all;
  for (const auto& [p, _] : a) all.insert(p);
  for (const auto& [p, _] : b) all.insert(p);
  std::string out;
  std::string err;
  for (const auto& path : all) {
    auto ia = a.find(path), ib = b.find(path);
    if (ia != a.end() && ib != b.end() && ia->second == ib->second) continue;
    std::string old_text = ia == a.end() ? "" : read(name, ra, path, &err).value_or("");
    std::string new_text = ib == b.end() ? "" : read(name, rb, path, &err).value_or("");
    if (!is_text(old_text) || !is_text(new_text)) {
      out += "Binary file " + path + " " + (ia == a.end() ? "added" : ib == b.end() ? "deleted" : "changed") + "\n";
      continue;
    }
    out += unified_diff(old_text, new_text, ia == a.end() ? "/dev/null" : "a/" + path, ib == b.end() ? "/dev/null" : "b/" + path);
    if (out.size() > (1 << 20)) {
      out += "... (the diff goes on past 1 MiB; ask for fewer paths)\n";
      break;
    }
  }
  return out;
}

void Store::set_deployed(const std::string& name, int64_t rev, int64_t now_ms) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, "UPDATE projects SET deployed_rev = ?, deployed_ms = ? WHERE name = ?");
  s.bind(1, rev).bind(2, now_ms).bind(3, name).run();
}

}  // namespace goober::source
