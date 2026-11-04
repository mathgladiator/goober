#pragma once

// Traffic rollups for the operator panel (design/machine/028.Panel.md § 4): a view derived
// from the access log and the egress log, kept in storage/operations/traffic.db. A tailer
// reads what the logs gained since its cursor; rows and cursor commit together, so every
// line is counted exactly once, across crashes and rotations.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

// Latency in 40 buckets, each √2 wider than the last: bucket 0 holds up to 32 µs, and the
// last holds everything past about 16 s. Rows sum, so any window has percentiles.
struct Histogram {
  static constexpr int kBuckets = 40;
  std::array<int64_t, kBuckets> counts{};

  static int bucket(int64_t us);
  // The largest latency bucket b holds, in µs.
  static int64_t upper(int b);
  void add(int64_t us) { ++counts[bucket(us)]; }
  void merge(const Histogram& other);
  int64_t total() const;
  // The upper edge of the bucket holding the q-th quantile (0 < q <= 1), or 0 when empty.
  int64_t percentile(double q) const;
  // "3,0,12": counts, trailing zeros left off.
  std::string encode() const;
  static Histogram decode(std::string_view text);
};

// "2026-10-29T13:09:41.090Z" as unix milliseconds, or nullopt.
std::optional<int64_t> parse_utc_ms(std::string_view text);

// Where one access-log line is counted.
struct LineKey {
  int64_t at_ms = 0;
  std::string tenant, box, route;
};
// The tenant a configured domain belongs to, or nullopt for a domain the host doesn't serve.
using TenantOf = std::function<std::optional<std::string>(const std::string& domain)>;
// Keys one parsed access-log line (§ 4), or nullopt for a line with no time.
std::optional<LineKey> key_line(const bj::object& line, const TenantOf& tenant_of);

// One per-domain control's five minutes (design/machine/047.Controls.md § 6), as the controls
// gate counts them: rows sum, sketches union, so any window adds up. Clients are never named:
// distinct ones are a 256-bit sketch (64 hex digits), and the busiest one a keyed hash.
struct ControlRow {
  int64_t t = 0;  // the bucket's start
  std::string tenant, domain, control;
  int64_t matched = 0, allowed = 0;
  int64_t rate = 0, concurrent = 0, body = 0, closed = 0;  // refused, by reason
  int64_t would = 0;                                       // would have refused (observe)
  int64_t n5 = 0;                                          // of the allowed, answered 5xx
  int64_t peak = 0;                                        // the most in flight at once
  std::string clients, would_clients;                      // sketches of distinct clients refused, would-refuse
  std::string top_client, would_top;                       // the busiest one's keyed hash
  int64_t top_n = 0, would_top_n = 0;
  Histogram hist;  // latency of the allowed
  int64_t refused() const { return rate + concurrent + body + closed; }
  void merge(const ControlRow& o);
};
// Two sketches of 64 hex digits, unioned; an empty one is nothing.
std::string sketch_union(const std::string& a, const std::string& b);

// The MCP queue's five minutes (design/machine/047.Controls.md § 13) for one queue (`mcp`, a
// domain's /mcp, or `operator`, the operator MCP) and one class of agent: what arrived, what
// waited and for how long, what ran, and what was refused and why. Agents are never named.
struct McpQueueRow {
  int64_t t = 0;               // the bucket's start
  std::string queue;           // mcp or operator
  std::string tenant, domain;  // "" for the operator MCP
  std::string cls;             // agent, person, unknown, or operator
  int64_t offered = 0;         // calls that arrived (each call of a batch counts)
  int64_t queued = 0;          // of the admitted, those that waited for a slot
  int64_t ran = 0;             // started, at once or after waiting
  int64_t refused_rate = 0, refused_full = 0, refused_wait = 0;  // the strict rate, a full queue, waited too long
  int64_t gone = 0;            // left while waiting (the client disconnected): removed, never run
  int64_t peak_waiting = 0, peak_running = 0;  // the most of this class waiting, and running, at once in its queue
  Histogram waited;            // how long the calls that waited waited (µs)
  int64_t refused() const { return refused_rate + refused_full + refused_wait; }
  void merge(const McpQueueRow& o);
};

class Traffic {
 public:
  // `logs` holds access.log, egress.log, and mail.log (and their rotations).
  Traffic(std::filesystem::path db_file, std::filesystem::path logs, TenantOf tenant_of);
  ~Traffic();
  Traffic(const Traffic&) = delete;
  Traffic& operator=(const Traffic&) = delete;

  // The bytes the rollups hold (free pages left out), and the budget past which the oldest
  // hours go (0: none). Safe from any thread.
  int64_t size_bytes();
  void set_budget(int64_t bytes);
  // Hours dropped to keep within the budget since the host started.
  int64_t budget_drops() const { return budget_drops_; }

  // Counts what the logs gained since the cursors, and drops rows past retention. Returns
  // the lines read. Safe from any thread.
  size_t catch_up(int64_t now_ms);

  // A window name: 1h, 12h, or 7d. Rows are five-minute buckets kept seven days (045.Limits.md).
  static bool valid_window(std::string_view window);

  // {window, step_ms, from_ms, to_ms, points: [{t, n, n3, n4, n5, us, p50_us, p99_us}]}.
  bj::object series(std::string_view window, const std::string& tenant, const std::string& box, int64_t now_ms);
  // [{tenant, box, route, n, n3, n4, n5, us, us_max, p50_us, p99_us, fuel, sql, outbox}], busiest first.
  bj::array routes(std::string_view window, const std::string& tenant, const std::string& box, int64_t now_ms);
  // [{box, n, n5, us, fuel, sql, share}], hottest (most CPU time) first; share is of all boxes' us.
  bj::array hot_boxes(std::string_view window, int64_t now_ms);
  // [{tenant, from_box, to_kind, to_name, n, errors, us, bytes_out, bytes_in}].
  bj::array edges(std::string_view window, const std::string& box, int64_t now_ms);
  // What visitors and clients got that wasn't a success (028 § 4.2): every 4xx and 5xx answered
  // to a request from outside (box-to-box calls and route_try left out), by who made it
  // (error_by: host, box, or box_failed). {window, step_ms, from_ms, to_ms, totals: {n, n4, n5,
  // host, box, box_failed}, points: [{t, n, n4, n5, host, box, box_failed}], statuses: [{status,
  // n, host, box, box_failed}], places: [{tenant, place, box, route, status, error_by, n, same,
  // other, none}] (most first, at most 200), referred: {same, other, none}}. A place is a box's
  // route, a page's path, a host area (identity, oauth, mcp, operations, attachment, ...), or
  // "(not found)" for every path nothing answers, so a scan can't push out the rest.
  bj::object errors(std::string_view window, const std::string& tenant, int64_t now_ms);

  // What went out to services (028 § 4.4), from the egress log (boxes' calls and outbox
  // deliveries, the host's push, robot fetches, and token refreshes) and the mail log (every
  // mail the mailer sent, as service "mailer", method the mailer: ses, smtp, or dev; tenant ""
  // for the host's own mail). {window, step_ms, from_ms, to_ms, totals: {calls, failed, n4, n5,
  // neterr, outbox, us, p50_us, p99_us, bytes_out, bytes_in}, points: [{t, calls, failed,
  // outbox, p50_us, p99_us}], services: [{service, ...the totals' fields, tenants, boxes}]
  // (most calls first), methods: [{tenant, box, service, method, ...the totals' fields}],
  // statuses: [{service, status, n}] (failures only; 0 is no answer), latest: [{at_ms, tenant,
  // box, service, method, status, error, url, request_id, outbox_id}] (failures, newest first,
  // at most `latest`)}. A failure is no answer, an error, or a status of 400 or more. Empty
  // tenant or service means every one.
  bj::object services(std::string_view window, const std::string& tenant, const std::string& service, int64_t now_ms,
                      size_t latest = 50);

  // A sampled level (028 § 4.4): name (like outbox_pending), a tenant, and its value per key
  // (like a service), all of them: a key sampled before and missing now reads 0. Kept in memory
  // and written by catch_up as each five minutes' largest value. Safe from any thread.
  void gauge(int64_t at_ms, const std::string& name, const std::string& tenant, const std::map<std::string, int64_t>& values);
  // {window, step_ms, from_ms, to_ms, points: [{t, value}], now: [{tenant, key, value, at_ms}]}:
  // per point, the largest sum over every tenant and key in five minutes; now holds the
  // latest sample of each. Empty tenant means every one.
  bj::object gauge_series(std::string_view window, const std::string& name, const std::string& tenant, int64_t now_ms);

  // Log volume (032 § 10): a line of `bytes` was written to `log` at at_ms, by the tenant, box, and
  // route the line names ("" when it names none). Kept in memory and written by catch_up: five-
  // minute rows for seven days, and one row per log per UTC day for 90 days. Safe from any thread.
  void log_written(int64_t at_ms, const std::string& log, const std::string& tenant, const std::string& box,
                   const std::string& route, int64_t bytes);
  // Each log's bytes per UTC day from the rotated files still in the logs directory, for days
  // before today the host didn't count (lines unknown). Run once as the store opens. Returns the
  // rows added.
  int seed_log_days(int64_t now_ms);
  // How fast each log grows (032 § 10): {window, step_ms, from_ms, to_ms, logs: [{log, last_24h_bytes,
  // last_24h_lines, today_bytes, median_day_bytes, days_known, growth, grew}], series: [{log,
  // points: [bytes per step]}], days: [{day, log, bytes, lines, counted}] (90 days, newest first),
  // top: [{log, tenant, box, route, bytes, lines, share}] (over the window, most first, at most
  // `top`)}. growth is the last 24 hours over the median of the seven UTC days before today
  // (null with fewer than three known); grew when it is at least kGrowthFactor and the last 24
  // hours hold at least kGrowthFloorBytes. Empty log means every one.
  static constexpr double kGrowthFactor = 3.0;
  static constexpr int64_t kGrowthFloorBytes = int64_t(1) << 20;
  bj::object log_velocity(std::string_view window, const std::string& log, int64_t now_ms, size_t top = 20);

  // Per-domain controls (047 § 6): five-minute rows the gate hands over, written by catch_up and
  // kept 7 days within traffic-bytes, like every other rollup. Safe from any thread.
  void add_control_rows(std::vector<ControlRow> rows);
  // Rows of a domain (and one control, or "" for all) with from_ms <= t < to_ms, oldest first.
  std::vector<ControlRow> control_rows(const std::string& domain, const std::string& control, int64_t from_ms, int64_t to_ms);
  // The MCP queue (047 § 13): five-minute rows the queue hands over, kept like the controls' rows.
  void add_mcp_queue_rows(std::vector<McpQueueRow> rows);
  // Rows of one queue (mcp with a domain, or "" for every domain; operator with "") with
  // from_ms <= t < to_ms, oldest first.
  std::vector<McpQueueRow> mcp_queue_rows(const std::string& queue, const std::string& domain, int64_t from_ms,
                                          int64_t to_ms);

 private:
  struct Window;
  size_t tail(const std::string& name, int64_t now_ms);
  void drop_old(int64_t now_ms);
  int64_t size_bytes_locked();
  int64_t budget_bytes_ = 0;
  std::atomic<int64_t> budget_drops_{0};

  std::filesystem::path logs_;
  TenantOf tenant_of_;
  void flush_gauges();
  void trim_service_failures(int64_t now_ms);
  bool service_failures_added_ = false;
  std::mutex mu_;
  std::unique_ptr<sqlite::Db> db_;
  // (name, tenant, key) -> (at_ms, value), the latest sample; and each five minutes' largest, unwritten.
  std::mutex gauge_mu_;
  std::map<std::tuple<std::string, std::string, std::string>, std::pair<int64_t, int64_t>> gauge_now_;
  std::map<std::tuple<int64_t, std::string, std::string, std::string>, int64_t> gauge_pending_;
  // (bucket, log, tenant, box, route) -> (bytes, lines), unwritten.
  void flush_logs();
  std::mutex log_mu_;
  std::map<std::tuple<int64_t, std::string, std::string, std::string, std::string>, std::pair<int64_t, int64_t>> log_pending_;
  void flush_controls();
  std::mutex control_mu_;
  std::vector<ControlRow> control_pending_;
  void flush_mcp_queue();
  std::mutex mcp_queue_mu_;
  std::vector<McpQueueRow> mcp_queue_pending_;
  int64_t last_drop_ms_ = 0;
};

}  // namespace goober::operations
