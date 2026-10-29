#pragma once

// Traffic rollups for the operator panel (design/machine/028.Panel.md § 4): a view derived
// from the access log and the egress log, kept in storage/operations/traffic.db. A tailer
// reads what the logs gained since its cursor; rows and cursor commit together, so every
// line is counted exactly once, across crashes and rotations.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

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

class Traffic {
 public:
  // `logs` holds access.log and egress.log (and their rotations, .1 to .3).
  Traffic(std::filesystem::path db_file, std::filesystem::path logs, TenantOf tenant_of);
  ~Traffic();
  Traffic(const Traffic&) = delete;
  Traffic& operator=(const Traffic&) = delete;

  // Counts what the logs gained since the cursors, and drops rows past retention. Returns
  // the lines read. Safe from any thread.
  size_t catch_up(int64_t now_ms);

  // A window name: 1h, 12h, 7d, or 30d.
  static bool valid_window(std::string_view window);

  // {window, step_ms, from_ms, to_ms, points: [{t, n, n3, n4, n5, us, p50_us, p99_us}]}.
  bj::object series(std::string_view window, const std::string& tenant, const std::string& box, int64_t now_ms);
  // [{tenant, box, route, n, n3, n4, n5, us, us_max, p50_us, p99_us, fuel, sql, outbox}], busiest first.
  bj::array routes(std::string_view window, const std::string& tenant, const std::string& box, int64_t now_ms);
  // [{box, n, n5, us, fuel, sql, share}], hottest (most CPU time) first; share is of all boxes' us.
  bj::array hot_boxes(std::string_view window, int64_t now_ms);
  // [{tenant, from_box, to_kind, to_name, n, errors, us, bytes_out, bytes_in}].
  bj::array edges(std::string_view window, const std::string& box, int64_t now_ms);

 private:
  struct Window;
  size_t tail(const std::string& name, int64_t now_ms);
  void drop_old(int64_t now_ms);

  std::filesystem::path logs_;
  TenantOf tenant_of_;
  std::mutex mu_;
  std::unique_ptr<sqlite::Db> db_;
  int64_t last_drop_ms_ = 0;
};

}  // namespace goober::operations
