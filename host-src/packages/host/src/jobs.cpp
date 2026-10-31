// Jobs (design/machine/038.Ids.md § 7): one-time boxes an operator starts, dry run first.

#include "jobs.h"

#include <sqlite3.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "goober/base/encoding.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"
#include "timer_state.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS runs (
  id INTEGER PRIMARY KEY, box TEXT NOT NULL, version TEXT NOT NULL, job TEXT NOT NULL, tenant TEXT NOT NULL,
  dry_run INTEGER NOT NULL, state TEXT NOT NULL, cursor TEXT NOT NULL, calls INTEGER NOT NULL, moved INTEGER NOT NULL,
  failures_in_row INTEGER NOT NULL, last_error TEXT NOT NULL, by TEXT NOT NULL, started_ms INTEGER NOT NULL,
  updated_ms INTEGER NOT NULL, finished_ms INTEGER NOT NULL, errors TEXT NOT NULL, log TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS runs_by_job ON runs (box, job, tenant);
)sql";

constexpr int kFailuresToStop = 3;
constexpr int64_t kMaxCalls = 1'000'000;
constexpr size_t kKeepErrors = 20, kKeepLog = 50;

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

void keep_newest(bj::array& a, bj::value v, size_t n) {
  a.push_back(std::move(v));
  while (a.size() > n) a.erase(a.begin());
}

std::string text_of(const bj::value& v) { return v.is_string() ? std::string(v.as_string()) : json::dump(v); }

}  // namespace

bj::object JobRun::to_json() const {
  bj::object o{{"run", std::to_string(id)}, {"box", box},       {"version", version},       {"job", job},
               {"tenant", tenant},          {"dry_run", dry_run}, {"state", state},          {"cursor", cursor},
               {"calls", calls},            {"moved", moved},   {"started_at", base::format_utc_ms(started_ms)},
               {"updated_at", base::format_utc_ms(updated_ms)}, {"by", by}};
  if (finished_ms) o["finished_at"] = base::format_utc_ms(finished_ms);
  if (!last_error.empty()) o["last_error"] = last_error;
  if (!errors.empty()) o["errors"] = errors;
  if (!log.empty()) o["log"] = log;
  return o;
}

JobRuns::JobRuns(const fs::path& file) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  db_ = std::make_unique<sqlite::Db>(file);
  db_->exec("PRAGMA journal_mode = WAL");
  db_->exec(kSchema);
}

void JobRuns::save(const JobRun& r) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_,
                 "INSERT OR REPLACE INTO runs (id, box, version, job, tenant, dry_run, state, cursor, calls, moved, "
                 "failures_in_row, last_error, by, started_ms, updated_ms, finished_ms, errors, log) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  s.bind(1, r.id).bind(2, r.box).bind(3, r.version).bind(4, r.job).bind(5, r.tenant).bind(6, int64_t(r.dry_run));
  s.bind(7, r.state).bind(8, r.cursor).bind(9, r.calls).bind(10, r.moved).bind(11, r.failures_in_row);
  s.bind(12, r.last_error).bind(13, r.by).bind(14, r.started_ms).bind(15, r.updated_ms).bind(16, r.finished_ms);
  s.bind(17, json::dump(r.errors)).bind(18, json::dump(r.log)).run();
}

std::optional<JobRun> JobRuns::read(sqlite::Stmt& s) {
  if (!s.step()) return std::nullopt;
  JobRun r;
  r.id = s.int64(0);
  r.box = s.text(1);
  r.version = s.text(2);
  r.job = s.text(3);
  r.tenant = s.text(4);
  r.dry_run = s.int64(5) != 0;
  r.state = s.text(6);
  r.cursor = s.text(7);
  r.calls = s.int64(8);
  r.moved = s.int64(9);
  r.failures_in_row = s.int64(10);
  r.last_error = s.text(11);
  r.by = s.text(12);
  r.started_ms = s.int64(13);
  r.updated_ms = s.int64(14);
  r.finished_ms = s.int64(15);
  std::string err;
  if (auto e = json::parse(s.text(16), &err); e && e->is_array()) r.errors = e->as_array();
  if (auto l = json::parse(s.text(17), &err); l && l->is_array()) r.log = l->as_array();
  return r;
}

constexpr const char* kColumns =
    "SELECT id, box, version, job, tenant, dry_run, state, cursor, calls, moved, failures_in_row, last_error, by, "
    "started_ms, updated_ms, finished_ms, errors, log FROM runs ";

std::optional<JobRun> JobRuns::get(int64_t id) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, std::string(kColumns) + "WHERE id = ?");
  s.bind(1, id);
  return read(s);
}

std::vector<JobRun> JobRuns::list(const std::string& box, const std::string& job, const std::string& tenant, int limit) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, std::string(kColumns) +
                           "WHERE (? = '' OR box = ?) AND (? = '' OR job = ?) AND (? = '' OR tenant = ?) "
                           "ORDER BY started_ms DESC, id DESC LIMIT ?");
  s.bind(1, box).bind(2, box).bind(3, job).bind(4, job).bind(5, tenant).bind(6, tenant).bind(7, int64_t(limit));
  std::vector<JobRun> out;
  while (auto r = read(s)) out.push_back(std::move(*r));
  return out;
}

std::vector<JobRun> JobRuns::running() {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, std::string(kColumns) + "WHERE state = 'running' ORDER BY id");
  std::vector<JobRun> out;
  while (auto r = read(s)) out.push_back(std::move(*r));
  return out;
}

bool JobRuns::dry_run_passed(const std::string& box, const std::string& version, const std::string& job,
                             const std::string& tenant) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, "SELECT 1 FROM runs WHERE box = ? AND version = ? AND job = ? AND tenant = ? AND dry_run = 1 "
                       "AND state = 'passed'");
  s.bind(1, box).bind(2, version).bind(3, job).bind(4, tenant);
  return s.step();
}

bool JobRuns::done(const std::string& box, const std::string& version, const std::string& job, const std::string& tenant) {
  std::lock_guard lock(mu_);
  sqlite::Stmt s(*db_, "SELECT 1 FROM runs WHERE box = ? AND version = ? AND job = ? AND tenant = ? AND dry_run = 0 "
                       "AND state = 'done'");
  s.bind(1, box).bind(2, version).bind(3, job).bind(4, tenant);
  return s.step();
}

// ---- The host's side ----

fs::path Host::job_copies(int64_t run) const { return config_.get()->storage / "tmp" / "jobs" / std::to_string(run); }

void Host::init_jobs() {
  try {
    job_runs_ = std::make_unique<JobRuns>(config_.get()->storage / "operations" / "jobs.db");
    // A dry run's copies were in tmp: one cut off by a restart can't go on.
    for (auto r : job_runs_->running()) {
      if (!r.dry_run) continue;
      r.state = "failed";
      r.last_error = "the host restarted during the dry run; run it again";
      r.finished_ms = r.updated_ms = base::now_unix_ms();
      job_runs_->save(r);
      std::error_code ec;
      fs::remove_all(job_copies(r.id), ec);
    }
  } catch (const std::exception& e) {
    stores_off_.push_back("jobs");
    startup_notes_.push_back(std::string("jobs off: storage/operations/jobs.db won't open (") + e.what() + ")");
  }
}

void Host::run_jobs() {
  std::unique_lock lock(timer_thread_mu_);
  while (!timers_stopping_) {
    lock.unlock();
    bool worked = false;
    try {
      // One batch of each running job in turn, so one long job doesn't starve another.
      if (job_runs_) {
        for (auto& r : job_runs_->running()) {
          if (timers_stopping_) break;
          job_step(r);
          worked = true;
        }
      }
      loop_beat("jobs");
    } catch (const std::exception& e) {
      std::cerr << "goober-host: jobs: " << e.what() << "\n";
      loop_beat("jobs", e.what());
    }
    lock.lock();
    if (!worked) job_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return timers_stopping_ || job_nudged_; });
    job_nudged_ = false;
  }
}

void Host::job_step(JobRun& r) {
  auto save = [&] {
    r.updated_ms = base::now_unix_ms();
    // An operator's job_stop during this batch wins: the batch's progress is kept, stopped.
    if (auto now = job_runs_->get(r.id); now && now->state == "stopped" && r.state == "running") {
      r.state = "stopped";
      r.finished_ms = now->finished_ms;
    }
    job_runs_->save(r);
  };
  auto finish = [&](const std::string& state, const std::string& why) {
    r.state = state;
    if (!why.empty()) r.last_error = why;
    r.finished_ms = base::now_unix_ms();
    save();
    if (r.dry_run) {
      std::error_code ec;
      fs::remove_all(job_copies(r.id), ec);
    }
  };
  // The box version the run was started on, and still active: a new version is a new job.
  auto active = boxes_->active();
  auto it = active->boxes.find(r.box);
  if (it == active->boxes.end()) return finish(r.dry_run ? "failed" : "paused", "box " + r.box + " isn't active any more");
  auto box = it->second;
  if (box->version != r.version) {
    return finish(r.dry_run ? "failed" : "paused", "box " + r.box + " is now version " + box->version +
                                                       ": a job runs on the version it started on; dry run it again");
  }
  const manifest::HttpRoute* route = nullptr;
  for (const auto& j : box->manifest.jobs) {
    if (j.name == r.job) route = &j;
  }
  if (!route) return finish("failed", "box " + r.box + " has no job " + r.job);
  if (r.calls >= kMaxCalls) return finish(r.dry_run ? "failed" : "paused", "more than " + std::to_string(kMaxCalls) + " batches");

  // A dry run's databases: copies of the tenant's, made before its first call.
  std::unique_ptr<db::Databases> copies;
  if (r.dry_run) {
    auto root = job_copies(r.id);
    copies = std::make_unique<db::Databases>(root);
    copies->set_id_source(next_id_);
    if (r.calls == 0) {
      std::error_code ec;
      fs::remove_all(root, ec);
      for (const auto& d : box->manifest.data) {
        if (!d.applies_schema()) continue;
        std::string tenant = d.tenant_for(r.tenant), database = d.database_for(r.tenant);
        if (!databases_->exists(tenant, database) || copies->exists(tenant, database)) continue;
        std::string err;
        fs::create_directories(copies->file(tenant, database).parent_path(), ec);
        if (!copy_database(databases_->file(tenant, database), copies->file(tenant, database), &err)) {
          return finish("failed", err);
        }
      }
    }
  }

  std::string domain;
  for (const auto& [tn, dm] : tenants_of(*config_.get(), box->manifest)) {
    if (tn == r.tenant || domain.empty()) domain = dm;
  }
  uint8_t rnd[8];
  base::os_random_fill(rnd);
  std::string request_id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
  int64_t call = r.calls + 1;
  bj::object body{{"tenant", r.tenant}, {"cursor", r.cursor}, {"dry_run", r.dry_run}, {"call", call}};
  bj::object doc{{"request_id", request_id}, {"method", "job"},     {"path", "job:" + r.job},
                 {"params", bj::object{}},   {"query", bj::object{}}, {"headers", bj::object{}},
                 {"body", json::dump(body)}, {"domain", domain},     {"tenant", r.tenant}, {"user", nullptr}};
  CallOptions options;
  options.timeout_ms = route->timeout_ms;
  options.databases = copies.get();
  options.dry_run = r.dry_run;
  auto out = call_box(boxes::RouteRef{box, route}, doc, r.tenant, request_id, std::nullopt, options);
  r.calls = call;
  for (const auto& line : out.result.logs) {
    keep_newest(r.log, bj::object{{"call", call}, {"level", line.level}, {"text", line.text}}, kKeepLog);
  }
  access_log_job(r, request_id, out);

  // What the box answered: {"cursor", "done", "moved"}, with a 200.
  std::string why;
  std::optional<bj::object> answer;
  if (out.result.outcome != runtime::wasm::CallResult::Outcome::kOk) {
    why = out.result.error.empty() ? std::string(json::dump(out.extra.at("outcome"))) : out.result.error;
  } else if (out.result.status != 200) {
    why = "the box answered " + std::to_string(out.result.status) + ": " + out.result.body.substr(0, 300);
  } else {
    std::string err;
    auto v = json::parse(out.result.body, &err);
    if (!v || !v->is_object()) {
      why = "the box's answer isn't a JSON object like {\"cursor\": \"...\", \"done\": false, \"moved\": 10}";
    } else {
      answer = v->as_object();
    }
  }
  bool done = false;
  if (answer) {
    const auto* c = answer->if_contains("cursor");
    const auto* d = answer->if_contains("done");
    done = d && d->is_bool() && d->as_bool();
    std::string next = c ? text_of(*c) : "";
    if (c && !c->is_string()) {
      why = "cursor must be a string";
    } else if (!done && next == r.cursor) {
      why = "the cursor didn't move, and the job isn't done: it would run the same batch for ever";
    } else {
      if (const auto* m = answer->if_contains("moved"); m && m->is_int64()) r.moved += m->as_int64();
      r.cursor = next.substr(0, 4096);
    }
  }
  if (!why.empty()) {
    keep_newest(r.errors, bj::object{{"call", call}, {"error", why}}, kKeepErrors);
    r.last_error = why;
    if (++r.failures_in_row >= kFailuresToStop) {
      return finish(r.dry_run ? "failed" : "paused",
                    std::to_string(kFailuresToStop) + " batches failed in a row; the last: " + why);
    }
    return save();
  }
  r.failures_in_row = 0;
  if (done) return finish(r.dry_run ? (r.errors.empty() ? "passed" : "failed") : "done",
                          r.dry_run && !r.errors.empty() ? "some batches failed on the way; see errors" : "");
  save();
}

void Host::access_log_job(const JobRun& r, const std::string& request_id, const BoxCall& call) {
  bj::object extra = call.extra;
  extra["job"] = r.job;
  extra["run"] = std::to_string(r.id);
  if (r.dry_run) extra["dry_run"] = true;
  server::http::Request req;
  req.method = "job";
  req.path = "job:" + r.job;
  req.headers.emplace_back("goober-request-id", request_id);
  access_log(req, call.result.status ? call.result.status : 500, base::monotonic_us(), "", json::dump(extra));
}

void Host::register_job_tools() {
  init_jobs();
  auto tenant_prop = mcp::string_property("The tenant whose databases the job works on, like playground.");
  auto box_prop = mcp::string_property("The box, as box_list names it.");
  auto job_prop = mcp::string_property("The job's route name, from the box's routes.job.");
  auto run_prop = mcp::string_property("The run's id, as job_dry_run or job_start answered it.");

  // Waits up to `ms` for a run to leave "running", then answers its state.
  auto wait_for = [this](int64_t id, int64_t ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
      auto r = job_runs_->get(id);
      if (!r || r->state != "running" || std::chrono::steady_clock::now() >= until) return r;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  };
  // Starts a run of an active box's job for a tenant, after the checks every start makes.
  auto start = [this, wait_for](const bj::object& args, bool dry_run) -> ToolResult {
    if (!job_runs_) return ToolResult::fail("jobs are off: see host_health's startup notes");
    auto box_name = arg(args, "box").value_or("");
    auto job = arg(args, "job").value_or("");
    auto tenant = arg(args, "tenant").value_or("");
    auto active = boxes_->active();
    auto it = active->boxes.find(box_name);
    if (it == active->boxes.end()) return ToolResult::fail("no active box named " + box_name);
    const auto& box = it->second;
    bool has = false;
    for (const auto& j : box->manifest.jobs) has |= j.name == job;
    if (!has) return ToolResult::fail("box " + box_name + " has no job named " + job + " (routes.job)");
    bool serves = false;
    for (const auto& [tn, dm] : tenants_of(*config_.get(), box->manifest)) serves |= tn == tenant;
    for (const auto& d : box->manifest.data) serves |= d.applies_schema() && d.tenant == tenant;
    if (!serves) return ToolResult::fail("box " + box_name + " doesn't serve tenant " + tenant);
    for (const auto& r : job_runs_->running()) {
      if (r.tenant == tenant) {
        return ToolResult::fail("run " + std::to_string(r.id) + " (" + r.box + "." + r.job + ") is running for " + tenant +
                                "; one job at a time per tenant");
      }
    }
    if (!dry_run) {
      if (!job_runs_->dry_run_passed(box_name, box->version, job, tenant)) {
        return ToolResult::fail("no dry run of " + box_name + " version " + box->version + "'s " + job + " for " + tenant +
                                " has passed: job_dry_run first, and read what it reports");
      }
      auto again = args.if_contains("again");
      if (job_runs_->done(box_name, box->version, job, tenant) && !(again && again->is_bool() && again->as_bool())) {
        return ToolResult::fail(job + " already ran to the end for " + tenant + " on this version; again: true runs it again");
      }
    }
    // A real run that paused or was stopped on this version goes on from its cursor.
    if (!dry_run) {
      auto again = args.if_contains("again");
      for (auto& old : job_runs_->list(box_name, job, tenant, 1)) {
        if (old.dry_run || old.version != box->version || (old.state != "paused" && old.state != "stopped")) break;
        if (again && again->is_bool() && again->as_bool()) break;
        old.state = "running";
        old.failures_in_row = 0;
        old.finished_ms = 0;
        old.last_error.clear();
        old.updated_ms = base::now_unix_ms();
        job_runs_->save(old);
        {
          std::lock_guard lock(timer_thread_mu_);
          job_nudged_ = true;
        }
        job_cv_.notify_all();
        auto now = wait_for(old.id, 25000);
        auto out = now ? now->to_json() : old.to_json();
        out["resumed"] = true;
        return ToolResult::ok(std::move(out));
      }
    }
    JobRun r;
    r.id = next_id_();
    r.box = box_name;
    r.version = box->version;
    r.job = job;
    r.tenant = tenant;
    r.dry_run = dry_run;
    r.by = current_operator();
    r.started_ms = r.updated_ms = base::now_unix_ms();
    job_runs_->save(r);
    {
      std::lock_guard lock(timer_thread_mu_);
      job_nudged_ = true;
    }
    job_cv_.notify_all();
    auto now = wait_for(r.id, 25000);
    auto out = now ? now->to_json() : r.to_json();
    if (now && now->state == "running") out["note"] = "still running: follow it with job_status";
    return ToolResult::ok(std::move(out));
  };

  mcp_->add_tool({"job_list",
                  "The jobs (routes.job) of the active boxes that serve a tenant, each with its newest runs: one-time "
                  "work, like moving data into a new database, that an operator starts. A job runs only after a dry "
                  "run of the same box version passed (job_dry_run), then with job_start.",
                  mcp::object_schema({{"tenant", tenant_prop}}, {"tenant"}), [this](const bj::object& args) {
                    if (!job_runs_) return ToolResult::fail("jobs are off: see host_health's startup notes");
                    auto tenant = arg(args, "tenant").value_or("");
                    bj::array out;
                    for (const auto& [name, box] : boxes_->active()->boxes) {
                      for (const auto& j : box->manifest.jobs) {
                        bj::array runs;
                        for (const auto& r : job_runs_->list(name, j.name, tenant, 5)) runs.push_back(r.to_json());
                        out.push_back(bj::object{{"box", name},
                                                 {"version", box->version},
                                                 {"job", j.name},
                                                 {"tenant", tenant},
                                                 {"dry_run_passed", job_runs_->dry_run_passed(name, box->version, j.name, tenant)},
                                                 {"done", job_runs_->done(name, box->version, j.name, tenant)},
                                                 {"runs", std::move(runs)}});
                      }
                    }
                    return ToolResult::ok(bj::object{{"tenant", tenant}, {"jobs", std::move(out)}});
                  }});
  mcp_->add_tool({"job_dry_run",
                  "Runs a box's job for a tenant against copies of the tenant's databases the box names, then throws "
                  "the copies away. Reports batches, rows the box said it moved, every error, and the box's log. "
                  "Internal calls to other boxes are refused; anything queued for delivery stays in the copies. A real "
                  "run (job_start) needs a passed dry run of the same box version. Answers within 25 s, or with a run "
                  "to follow with job_status.",
                  mcp::object_schema({{"box", box_prop}, {"job", job_prop}, {"tenant", tenant_prop}}, {"box", "job", "tenant"}),
                  [start](const bj::object& args) { return start(args, true); }});
  mcp_->add_tool({"job_start",
                  "Runs a box's job for a tenant, for real, in batches: the box answers each call with "
                  "{\"cursor\", \"done\", \"moved\"}, and the host keeps the cursor after each commit, so a restart "
                  "resumes. Needs a passed dry run of the same box version (job_dry_run). Pauses after 3 failed batches "
                  "in a row; job_start again goes on from the cursor of a paused or stopped run. A job that ran to the "
                  "end runs again only with again: true.",
                  mcp::object_schema({{"box", box_prop},
                                      {"job", job_prop},
                                      {"tenant", tenant_prop},
                                      {"again", mcp::boolean_property("Run a job that already finished, again.")}},
                                     {"box", "job", "tenant"}),
                  [start](const bj::object& args) { return start(args, false); }});
  mcp_->add_tool({"job_status", "One run of a job: its state, cursor, batches, rows moved, errors, and the box's log.",
                  mcp::object_schema({{"run", run_prop}}, {"run"}), [this, wait_for](const bj::object& args) {
                    if (!job_runs_) return ToolResult::fail("jobs are off: see host_health's startup notes");
                    int64_t id = std::atoll(arg(args, "run").value_or("0").c_str());
                    auto r = wait_for(id, 5000);
                    if (!r) return ToolResult::fail("no run " + std::to_string(id));
                    return ToolResult::ok(r->to_json());
                  }});
  mcp_->add_tool({"job_stop",
                  "Stops a running job after its current batch. A real run keeps its cursor: job_start on the same box "
                  "version goes on from it (again: true starts over from the beginning).",
                  mcp::object_schema({{"run", run_prop}, {"tenant", tenant_prop}}, {"run", "tenant"}),
                  [this](const bj::object& args) {
                    if (!job_runs_) return ToolResult::fail("jobs are off: see host_health's startup notes");
                    int64_t id = std::atoll(arg(args, "run").value_or("0").c_str());
                    auto r = job_runs_->get(id);
                    if (!r || r->tenant != arg(args, "tenant").value_or("")) return ToolResult::fail("no run " + std::to_string(id) + " for that tenant");
                    if (r->state != "running") return ToolResult::fail("run " + std::to_string(id) + " is " + r->state);
                    // Saved by the job thread's next look, which sees the state and skips it.
                    r->state = "stopped";
                    r->finished_ms = r->updated_ms = base::now_unix_ms();
                    job_runs_->save(*r);
                    if (r->dry_run) {
                      std::error_code ec;
                      fs::remove_all(job_copies(r->id), ec);
                    }
                    return ToolResult::ok(r->to_json());
                  }});
}

}  // namespace goober::host
