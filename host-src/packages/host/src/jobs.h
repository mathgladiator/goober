#pragma once

// Jobs (design/machine/038.Ids.md § 7), private to the host package: one-time work a box
// declares under routes.job, like moving data into a new database, that only an operator
// starts. The host calls the route in batches, each its own request, and keeps the cursor the
// box answers after every commit. A dry run of the same box version, job, and tenant, against
// copies of the box's databases, must pass before the real run. Runs are kept in
// storage/operations/jobs.db, so a restart resumes a real run where it was.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "goober/json/json.h"
#include "goober/sqlite/sqlite.h"

namespace goober::host {

struct JobRun {
  int64_t id = 0;
  std::string box, version, job, tenant;
  bool dry_run = false;
  // running, then: passed (a dry run that finished), done (a real run that finished), failed
  // (a dry run that didn't), paused (a real run after three failures in a row, or a new box
  // version), or stopped (by an operator).
  std::string state = "running";
  std::string cursor;
  int64_t calls = 0, moved = 0, failures_in_row = 0;
  std::string last_error, by;
  int64_t started_ms = 0, updated_ms = 0, finished_ms = 0;
  boost::json::array errors;  // the newest 20: {call, error}
  boost::json::array log;     // the box's log lines, the newest 50: {call, level, text}
  boost::json::object to_json() const;
};

class JobRuns {
 public:
  explicit JobRuns(const std::filesystem::path& file);
  void save(const JobRun& run);
  std::optional<JobRun> get(int64_t id);
  // Newest first; empty strings match anything.
  std::vector<JobRun> list(const std::string& box, const std::string& job, const std::string& tenant, int limit);
  std::vector<JobRun> running();
  // A dry run of this version, job, and tenant that passed.
  bool dry_run_passed(const std::string& box, const std::string& version, const std::string& job, const std::string& tenant);
  // A real run of this version, job, and tenant that finished.
  bool done(const std::string& box, const std::string& version, const std::string& job, const std::string& tenant);

 private:
  std::optional<JobRun> read(sqlite::Stmt& s);
  std::mutex mu_;
  std::unique_ptr<sqlite::Db> db_;
};

}  // namespace goober::host
