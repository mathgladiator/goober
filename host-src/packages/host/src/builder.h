#pragma once

// Building source projects (design/human/521.Source.md; design/machine/020.Source.md §4),
// private to the host package. A build reads goober.project.json, compiles each box (Rust
// with cargo in a bubblewrap sandbox, WebAssembly text, or a committed box.wasm), and gathers
// the schemas and site files a deploy needs. Builds run one at a time on their own thread, as
// jobs operators can follow.

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "goober/config/config.h"
#include "goober/json/json.h"
#include "goober/source/source.h"

namespace goober::host {

struct BuiltBox {
  std::string dir;       // in the project, like "." or "boxes/leads"
  std::string box;       // the manifest's box name
  std::string manifest;  // its text
  std::string module;    // WASM bytes, or WAT text
  std::string types;     // the types document's text, or ""
};

struct BuiltSite {
  std::string dir, tenant;
  std::vector<std::pair<std::string, std::string>> files;  // path under the site dir, bytes
};

struct BuildResult {
  bool ok = false;
  std::string log;  // what the build said, compiler errors included (the end, when long)
  std::vector<std::pair<std::string, std::string>> schemas;  // file, text
  std::vector<BuiltBox> boxes;
  std::vector<BuiltSite> sites;
};

// Builds a project's revision in `work` (which it empties first). `cache` keeps cargo's target
// directory between builds of the project, so a rebuild compiles only what changed.
BuildResult build_project(source::Store& store, const std::string& project, int64_t rev, const config::HostConfig& cfg,
                          const std::filesystem::path& work, const std::filesystem::path& cache);

// The sandbox command a Rust build runs under: bwrap with no network, the toolchain and the
// vendored crates read-only, and only the project and the cache writable. Exposed for tests.
std::vector<std::string> sandboxed(const std::vector<std::string>& command, const config::HostConfig& cfg,
                                   const std::filesystem::path& src, const std::filesystem::path& target,
                                   const std::string& cargo, const std::string& rustup_home, std::string* error);

// Files for a new project (source_create's template): "empty", "rust-box", "wat-box", "site".
std::optional<std::vector<source::Change>> template_files(const std::string& kind, const std::string& project,
                                                          const std::string& tenant, std::string* error);

struct BuildJob {
  int64_t id = 0;
  std::string project, kind;  // build or deploy
  int64_t rev = 0;
  std::string state = "queued";  // queued, running, succeeded, failed
  std::string author;
  int64_t queued_ms = 0, started_ms = 0, finished_ms = 0;
  std::string log;
  boost::json::object result;
};

class BuildJobs {
 public:
  using Job = BuildJob;
  using Work = std::function<void(Job&)>;
  BuildJobs();
  ~BuildJobs();
  // Queues work; it runs after every job queued before it.
  int64_t submit(Job job, Work work);
  // The job, waiting up to wait_ms for it to finish.
  std::optional<Job> get(int64_t id, int64_t wait_ms);
  std::vector<Job> recent();

 private:
  void run();
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::pair<int64_t, Work>> queue_;
  std::map<int64_t, Job> jobs_;
  int64_t next_id_ = 1;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace goober::host
