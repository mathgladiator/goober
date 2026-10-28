#pragma once

// Source projects kept in the host (design/human/521.Source.md; design/machine/020.Source.md):
// a straight line of numbered revisions per project, each a full snapshot of its files, with
// file contents stored once by SHA-256. A commit names the revision it started from; if others
// committed since, it lands on top only when none of their files are its files.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace goober::sqlite {
class Db;
}

namespace goober::source {

inline constexpr size_t kMaxFileBytes = 4 << 20;
inline constexpr size_t kMaxFiles = 10000;
inline constexpr size_t kMaxProjectBytes = 256 << 20;
inline constexpr size_t kMaxPathBytes = 400;

struct Project {
  std::string name, description;
  int64_t head = 0;  // 0 until the first commit
  int64_t created_ms = 0, updated_ms = 0;
  int64_t deployed_rev = 0, deployed_ms = 0;  // 0 when never deployed
};

struct FileEntry {
  std::string path, sha256;
  int64_t size = 0;
};

struct Change {
  std::string path;
  std::optional<std::string> content;  // nullopt deletes the file
};

struct Revision {
  int64_t rev = 0;
  std::string message, author;
  int64_t at_ms = 0;
  std::vector<std::string> changed;  // paths written or deleted
};

struct CommitResult {
  int64_t rev = 0;
  bool rebased = false;               // others had committed since base, on other files
  std::vector<std::string> conflicts;  // when refused: the files both touched
};

// A project name is the host's name grammar: [a-z][a-z0-9_]*, plus hyphens inside.
bool valid_project_name(std::string_view name);
// A path is relative, "/"-separated, without empty, ".", or ".." segments, NUL, or backslash.
bool valid_path(std::string_view path);
// Text a person or a model can read: valid UTF-8 without NUL.
bool is_text(std::string_view bytes);

class Store {
 public:
  explicit Store(std::filesystem::path file);
  ~Store();

  bool create(const std::string& name, const std::string& description, int64_t now_ms, std::string* error);
  std::vector<Project> list();
  std::optional<Project> get(const std::string& name);

  // The files at a revision (0 for the head), sorted by path.
  std::optional<std::vector<FileEntry>> files(const std::string& name, int64_t rev, std::string* error);
  std::optional<std::string> read(const std::string& name, int64_t rev, const std::string& path, std::string* error);

  // Commits changes made on top of `base`. Refused (nullopt, with `conflicts` filled when that's
  // why) when base isn't a revision, when a later revision touched the same files, or past the
  // limits.
  std::optional<CommitResult> commit(const std::string& name, int64_t base, const std::vector<Change>& changes,
                                     const std::string& message, const std::string& author, int64_t now_ms,
                                     std::string* error, std::vector<std::string>* conflicts = nullptr);
  // A new revision with `rev`'s files.
  std::optional<int64_t> revert(const std::string& name, int64_t rev, const std::string& author, int64_t now_ms,
                                std::string* error);
  // Newest first.
  std::vector<Revision> log(const std::string& name, int limit);
  // A unified diff from one revision to another (0 for the head), optionally of some paths.
  std::optional<std::string> diff(const std::string& name, int64_t from, int64_t to, const std::vector<std::string>& paths,
                                  std::string* error);

  void set_deployed(const std::string& name, int64_t rev, int64_t now_ms);

 private:
  std::optional<int64_t> resolve(const std::string& name, int64_t rev, std::string* error);
  std::mutex mu_;
  std::unique_ptr<sqlite::Db> db_;
};

// The unified diff of two texts (3 lines of context), "" when they are equal. Past a size the
// diff is whole-file.
std::string unified_diff(std::string_view a, std::string_view b, const std::string& a_name, const std::string& b_name);

}  // namespace goober::source
