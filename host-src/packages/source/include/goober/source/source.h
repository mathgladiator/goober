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
// Over a project's whole history (every distinct file it has ever held), and projects a host.
inline constexpr size_t kMaxHistoryBytes = size_t(1) << 30;
inline constexpr size_t kMaxProjects = 200;

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

// A build or deploy job, kept once it finishes (020 § 9): the panel and source_job read it back.
struct JobRecord {
  int64_t id = 0;
  std::string project, kind;  // kind: build or deploy
  int64_t rev = 0;
  std::string state, author;  // state: succeeded or failed (queued and running live in memory)
  int64_t queued_ms = 0, started_ms = 0, finished_ms = 0;
  std::string log;
  std::string result;  // the job's result object, as JSON text
};

// A deploy that went live: which revision, by whom, and the box versions it uploaded, so a box's
// version history leads back to its source.
struct Deploy {
  int64_t job = 0;
  std::string project;
  int64_t rev = 0;
  int64_t at_ms = 0;
  std::string author;
  std::vector<std::pair<std::string, std::string>> boxes;  // box, version
  std::string message;  // the revision's message (read back, not stored again)
};

// Something the source mirror (020 § 10) has yet to put in the bucket: a revision's patch, or the
// tree of a revision that was deployed.
struct MirrorItem {
  std::string project;
  int64_t rev = 0;
  std::string kind;  // "patch" or "snapshot"
};

struct MirrorState {
  int64_t patches = 0, patch_rev = 0;  // how many revisions' patches are in the bucket, and the newest
  int64_t snapshots = 0, snapshot_rev = 0;
  int64_t pending = 0;  // patches and snapshots still to go
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

  // One revision's message, author, time, and changed paths.
  std::optional<Revision> revision(const std::string& name, int64_t rev);
  // The revision as `git format-patch` writes it, from the revision before (an empty tree for
  // the first): `git am` replays a project's patches, in order, into a git repository. Binary
  // files are git binary patches, so every byte comes through.
  std::optional<std::string> patch(const std::string& name, int64_t rev, std::string* error);

  // Jobs and deploys, kept for good (020 § 9).
  void save_job(const JobRecord& job);
  std::optional<JobRecord> job(int64_t id);
  // Newest first; every project's when `project` is "". Logs are left out.
  std::vector<JobRecord> jobs(const std::string& project, int limit);
  int64_t max_job_id();
  void record_deploy(const Deploy& deploy);
  // Newest first.
  std::vector<Deploy> deploys(const std::string& project, int limit);
  std::vector<Deploy> deploys_of_box(const std::string& box, int limit);
  // The deploy that uploaded this version of a box, if a source deploy did.
  std::optional<Deploy> deploy_of_version(const std::string& box, const std::string& version);

  // The mirror's queue (020 § 10): oldest first, patches before snapshots.
  std::vector<MirrorItem> mirror_pending(int limit);
  void mark_mirrored(const MirrorItem& item, int64_t now_ms);
  MirrorState mirror_state(const std::string& project);

 private:
  std::optional<int64_t> resolve(const std::string& name, int64_t rev, std::string* error);
  std::vector<Deploy> read_deploys(const std::string& where, const std::string& arg, int limit);
  std::mutex mu_;
  std::unique_ptr<sqlite::Db> db_;
};

// The unified diff of two texts (3 lines of context), "" when they are equal. Past a size the
// diff is whole-file.
std::string unified_diff(std::string_view a, std::string_view b, const std::string& a_name, const std::string& b_name);

// Git's pieces, for patches: a blob's object id, and a file's bytes as the lines of a `GIT
// binary patch` literal (a zlib stream of stored blocks, in git's base85 lines).
std::string git_blob_id(std::string_view content);
std::string git_binary_literal(std::string_view content);

}  // namespace goober::source
