#pragma once

// Live statistics (design/machine/032.Logs.md § 6): per tenant and path, the last hour in
// twelve 5-minute buckets (count, 4xx, 5xx, max, and a 40-bucket latency histogram), and the
// flow between paths from same-site Referers. Kept in memory as the access log is written, so a
// p99 needs no tailer; bounded, least recently seen first out; lost on restart.

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace goober::operations {

class LiveStats {
 public:
  static constexpr int kBuckets = 12;               // 5 minutes each: one hour
  static constexpr int64_t kBucketMs = 5 * 60 * 1000;
  explicit LiveStats(size_t max_paths = 10000, size_t max_flows = 20000);

  // One request. `from` is the same-site referring path, or "".
  void record(int64_t now_ms, const std::string& tenant, const std::string& path, const std::string& route,
              const std::string& box, int status, int64_t us, const std::string& from);

  struct Row {
    std::string tenant, path, route, box;
    int64_t n = 0, n4 = 0, n5 = 0, max_us = 0, p50_us = 0, p95_us = 0, p99_us = 0;
  };
  // Every path seen in the last `minutes` (5 to 60), for one tenant or all, under a prefix.
  std::vector<Row> stats(int64_t now_ms, int minutes, const std::string& tenant, const std::string& prefix) const;

  struct Flow {
    std::string from, to;
    int64_t n = 0;
  };
  // The flow into and out of a path (or every flow, for an empty path), most first.
  std::vector<Flow> flows(int64_t now_ms, int minutes, const std::string& tenant, const std::string& path) const;
  size_t paths() const;

 private:
  struct Bucket {
    int64_t at = -1;  // which 5 minutes (ms / kBucketMs), or -1
    uint32_t n = 0, n4 = 0, n5 = 0;
    int64_t max = 0;
    std::array<uint32_t, 40> hist{};
  };
  struct Entry {
    std::string route, box;
    std::array<Bucket, kBuckets> ring{};
    std::list<std::string>::iterator lru;
  };
  struct FlowEntry {
    std::array<std::pair<int64_t, uint32_t>, kBuckets> ring{};
    std::list<std::string>::iterator lru;
  };
  mutable std::mutex mu_;
  size_t max_paths_, max_flows_;
  std::map<std::string, Entry> entries_;  // tenant \n path
  std::list<std::string> order_;          // most recent at the front
  std::map<std::string, FlowEntry> flows_;  // tenant \n from \n to
  std::list<std::string> flow_order_;
};

}  // namespace goober::operations
