#pragma once

// How boxes fail (design/machine/028.Panel.md § 4.3): the host's own record of every box call,
// whatever started it (a request, a tool, a timer, mail, a job, another box), and of each way it
// went wrong, kept in storage/operations/failures.db. Counts per minute (48 hours) and per hour
// (400 days) by tenant, box, route, kind, and class, and the latest messages per box (200, for
// 30 days). Recorded in memory as calls finish and written by the traffic loop, so a call never
// waits on this database; what wasn't written yet is lost on a crash, as live statistics are.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "goober/json/json.h"

namespace goober::sqlite {
class Db;
}

namespace goober::operations {

namespace bj = boost::json;

class Failures {
 public:
  // The classes, each in one group:
  //   call: the call failed. trap, timeout, out_of_fuel, failed (the box said so), no_response,
  //     bad_response (not its declared type), bad_status (a 5xx or a redirect off the site),
  //     render_failed (its template), outbox_failed, host_write_failed, commit_failed, host_error.
  //   sql: statements the host refused or SQLite failed, counted per statement, whether or not
  //     the call went on to succeed. sql_error, no_clock, refused_write (a write in a read).
  //   outbox: a delivery the box queued went wrong. delivery_retry, delivery_dead.
  //   internal: a call to another box failed, counted on the caller. internal_failed.
  //   answer: what a mail route chose, not a fault. mail_rejected, mail_deferred.
  // "" for a class this doesn't know.
  static std::string_view group_of(std::string_view cls);

  explicit Failures(std::filesystem::path db_file);
  ~Failures();
  Failures(const Failures&) = delete;
  Failures& operator=(const Failures&) = delete;

  // One box call ran. kind is the route's: get, post, ..., tool, timer, mail, job, internal,
  // identity, result, subscribe.
  void call(int64_t at_ms, const std::string& tenant, const std::string& box, const std::string& route,
            const std::string& kind);
  struct Event {
    int64_t at_ms = 0;
    std::string tenant, box, version, route, kind, cls, message, request_id;
    int64_t n = 1;  // more than one for statements
  };
  void failure(Event e);
  // Writes what's pending, and drops what's past keeping. Safe from any thread.
  void flush(int64_t now_ms);

  // {window, step_ms, from_ms, to_ms, groups: {class: group}, totals: {calls, failures, classes:
  // {class: n}}, points: [{t, calls, failures, sql, outbox, internal, answer}], routes: [{tenant,
  // box, route, kind, calls, failures, classes}] (most failures first), latest: [{at_ms, tenant,
  // box, version, route, kind, class, message, request_id}] (newest first, at most `latest`)}.
  // Windows are 1h, 12h, 7d, and 30d. Empty tenant or box means every one.
  bj::object report(std::string_view window, const std::string& tenant, const std::string& box, int64_t now_ms,
                    size_t latest = 50);

 private:
  // t (a minute, or -hour-1), tenant, box, route, kind, class ("" counts calls)
  using Key = std::tuple<int64_t, std::string, std::string, std::string, std::string, std::string>;
  void add(int64_t at_ms, const std::string& tenant, const std::string& box, const std::string& route,
           const std::string& kind, const std::string& cls, int64_t n);

  std::mutex pending_mu_;
  std::map<Key, int64_t> pending_;
  std::vector<Event> pending_events_;
  size_t dropped_events_ = 0;

  std::mutex db_mu_;
  std::unique_ptr<sqlite::Db> db_;
  int64_t last_drop_ms_ = 0;
};

}  // namespace goober::operations
