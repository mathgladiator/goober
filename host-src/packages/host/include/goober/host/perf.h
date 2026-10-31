#pragma once

// Performance recording (design/machine/037.Caching.md § 7): where each call's time goes,
// summed per route in memory for the last hour, and what each SQL statement costs, so an
// operator (or an agent) can see where a product is slow and what would fix it.

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "goober/db/database.h"

namespace goober::host {

// One call's numbers (037 § 7.1). The box's own time leaves out the host calls it made.
struct CallPerf {
  std::string tenant, box, route, verb;  // verb: get, post, ..., tool, timer, subscribe, job, mail, ...
  int status = 0;
  bool failed = false;  // the box failed or answered 5xx
  int64_t total_us = 0;       // from leaving the queue to the answer (queue time is apart)
  int64_t queue_us = 0;       // waiting for a box thread
  int64_t box_us = 0;         // running the box, its host calls left out
  int64_t sql_us = 0;         // inside goober_sql: lock + statements + the host's own checks
  int64_t lock_us = 0;        // of that, waiting for a database's write lock (BEGIN IMMEDIATE)
  int64_t statement_us = 0;   // of that, stepping statements
  int64_t commit_us = 0;      // committing, after the box answered
  int64_t out_us = 0;         // calls out (services, other boxes, libraries aside)
  int64_t render_us = 0;      // templates and partials
  int64_t bytes_out = 0;
  int64_t statements = 0;
  int64_t refused_writes = 0;  // statements refused because the call is a read (037 § 2)
  // The cache's answer for this call: "", "hit", "miss", "stale", "304", or "check"
  // (filled by the cache, 037 § 4–5).
  std::string cache;
  std::vector<std::string> databases_read, databases_written;  // "tenant/database"
  boost::json::object to_json() const;  // the access log's "perf"
};

class PerfStats {
 public:
  void record_call(const CallPerf& call, int64_t now_ms);
  void record_statements(const std::string& box, const std::string& route, const std::vector<db::StatementRecord>& done,
                         int64_t now_ms);
  // A subscription group evaluated (036 § 5); `changed` when its answer differed from the last.
  void record_subscription(const std::string& tenant, const std::string& box, const std::string& route, bool changed,
                           int64_t now_ms);
  // The cache's word on one call: "hit", "miss", "stale", "304", "check", or "mismatch" (a
  // sampled check whose bytes differed from the entry's, 037 § 4.3).
  void record_cache(const std::string& tenant, const std::string& box, const std::string& route,
                    const std::string& answer, int64_t now_ms);

  // perf_report: per route over the window, and findings with their evidence. Empty tenant
  // or box means every one.
  boost::json::object report(const std::string& tenant, const std::string& box, int64_t window_ms, int64_t now_ms);
  // perf_statements over the last hour, sorted by "total" (time), "count", "fullscan", or "max".
  boost::json::array statements(const std::string& tenant, const std::string& box, const std::string& sort, int limit,
                                int64_t now_ms);

  // Whitespace runs folded to one space, trimmed: the key a statement is counted under.
  static std::string normalize_sql(std::string_view sql);
  // For a statement that scans: its table, and the columns its WHERE and ORDER BY name, the
  // columns an index would want (a cheap reading, not a parser). Empty when it can't tell.
  static std::vector<std::string> index_columns(std::string_view sql, std::string* table);

  static constexpr size_t kMaxStatements = 2000;
  static constexpr int kHistogram = 40;

 private:
  struct Bucket {
    int64_t calls = 0, failed = 0;
    int64_t queue_us = 0, total_us = 0, box_us = 0, sql_us = 0, lock_us = 0, statement_us = 0, commit_us = 0,
            out_us = 0, render_us = 0, bytes_out = 0, statements = 0, refused_writes = 0;
    int64_t cache_hit = 0, cache_miss = 0, cache_stale = 0, cache_304 = 0, cache_check = 0, cache_mismatch = 0;
    int64_t evaluations = 0, changed = 0;
    std::array<int64_t, kHistogram> hist{};
    void add(const Bucket& o);
  };
  struct Route {
    std::string tenant, box, route, verb;
    std::map<int64_t, Bucket> minutes;  // by minute
    std::vector<std::string> databases_read;  // as last seen
  };
  struct Slot {
    int64_t count = 0, total_us = 0, max_us = 0, rows = 0, vm_steps = 0, fullscan_steps = 0, sorts = 0, autoindexes = 0;
  };
  struct Statement {
    std::string tenant, box, strategy, database, sql;
    std::vector<std::string> routes;  // up to 8 that ran it
    std::map<int64_t, Slot> slots;    // by ten-minute slot, the last six kept
    int64_t last_ms = 0;
    Slot sum(int64_t now_ms) const;
  };
  Route& route_for(const std::string& tenant, const std::string& box, const std::string& route, const std::string& verb);
  void prune(int64_t now_ms);

  std::mutex mu_;
  std::map<std::string, Route> routes_;
  std::map<std::string, std::map<int64_t, int64_t>> writes_;  // "tenant/database" -> minute -> calls that wrote
  std::map<std::string, Statement> statements_;
  int64_t pruned_ms_ = 0;
};

}  // namespace goober::host
