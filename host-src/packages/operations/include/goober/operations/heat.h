#pragma once

// Heat studies (design/machine/048.HeatStudy.md): a time-boxed look at one domain's traffic,
// keyed by endpoint, kept in memory while it runs and written as a small report when it ends.
// Each endpoint's heat (time spent serving), weight (bytes), rate (per minute, per client), and
// concurrency, with suggested controls in 047's vocabulary, always in observe mode. Nothing
// private: clients leave memory only as the host's keyed log hash, with counts.

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
#include <vector>

#include <boost/json.hpp>

#include "goober/operations/traffic.h"

namespace goober::operations {

// A box route's declared pattern as 047's match: "GET /posts/{id}" from get and
// "/posts/{id : int64}". The method is uppercased; types are left out.
std::string endpoint_match(std::string_view method, std::string_view pattern);

// Counts from 0 to about 2 million in 184 buckets: exact below 64, then eight per power of
// two (within 12.5%). A percentile answers its bucket's upper edge.
class CountHistogram {
 public:
  static constexpr int kBuckets = 184;
  void add(uint32_t v);
  void merge(const CountHistogram& o);
  uint32_t percentile(double q) const;  // 0 when empty
  uint32_t max() const { return max_; }
  uint64_t total() const { return n_; }
  static int bucket(uint32_t v);
  static uint32_t upper(int b);

 private:
  std::array<uint32_t, kBuckets> counts_{};
  uint32_t max_ = 0;
  uint64_t n_ = 0;
};

// HyperLogLog with 256 registers: about 6.5% error, 256 bytes.
class DistinctCounter {
 public:
  void add(uint64_t hash);
  void merge(const DistinctCounter& o);
  int64_t estimate() const;

 private:
  std::array<uint8_t, 256> reg_{};
};

// Which of 047's APIs a request was on. The host's own paths (/goober/*, CORS preflights) are
// never studied.
enum class HeatApi : uint8_t { kBox, kContent, kMcp, kIdentity };

// One finished request on a studied domain. Views, valid for the call: the study builds an
// endpoint's strings once, when it first sees the endpoint, and a client's keyed hash once per
// endpoint it asks.
struct HeatSample {
  int64_t at_ms = 0;   // wall clock at the finish
  int64_t end_us = 0;  // monotonic µs at the finish
  int64_t us = 0;      // how long it took
  HeatApi api = HeatApi::kContent;
  std::string_view box;     // the box's name, for kBox
  std::string_view method;  // lowercase ("get" for a HEAD); empty for an MCP call and a group
  // The route's declared pattern (kBox), the path (kContent, kIdentity), the tool's name or
  // "(rpc)" (kMcp), or a group: "(not found)", "(refused)".
  std::string_view what;
  std::string_view client;  // the client as the rate limiter counts it (an IPv6 /64 is one)
  int status = 0;
  int64_t bytes_in = 0, bytes_out = 0;
};

// What one endpoint's suggestion is computed from.
struct SuggestInput {
  int64_t study_id = 0;
  std::string domain, api, endpoint;
  int64_t requests = 0;
  int64_t clients = 0;
  uint32_t p99_client_minute = 0;  // the p99 of each client's busiest minute
  uint32_t peak_minute = 0;        // the endpoint's busiest minute, every client together
  int64_t peak_concurrent = 0, peak_client_concurrent = 0;
  double heat_share = 0;           // of the domain's heat
  double top_client_share = 0;     // the busiest client's share of the endpoint's requests
  int64_t max_body_bytes = 0;
  int64_t max_us = 0;
};
// {control_set: {...}, reasons: {...}} with mode "observe", or {none: why}.
boost::json::object suggest_controls(const SuggestInput& in);

struct Study;

class HeatStudies {
 public:
  static constexpr int kMaxRunning = 4;
  static constexpr int kMinMinutes = 5, kMaxMinutes = 1440, kDefaultMinutes = 60;
  static constexpr int64_t kKeepMs = int64_t(30) * 24 * 3600 * 1000;
  static constexpr int64_t kSaveEveryMs = 60 * 1000;
  static constexpr size_t kReportMaxBytes = 256 * 1024;
  static constexpr size_t kEndpointNameMax = 200;  // bytes of a sample's `what` kept
  struct Caps {
    size_t endpoints = 500;         // tracked; the rest are folded into "other"
    size_t candidates = 5000;       // untracked endpoints counted, to promote the busy ones
    size_t endpoint_clients = 256;  // clients kept per endpoint before idle ones are folded
    size_t endpoint_ring = 64;      // concurrency is exact up to this many at once per endpoint
    size_t detailed = 100;          // endpoints with every number and a suggestion in the report
  };
  // The client's keyed hash as the report shows it: the host's log hash of the rate-limit key,
  // which equals the audit log's client_h only for an IPv4 client (an IPv6 one is its /64).
  using ClientHash = std::function<uint64_t(std::string_view client)>;

  // `dir` holds one <id>.json per study. A study found "running" was cut short by a crash or a
  // kill: it becomes "interrupted", ending at its last save. Without `hash`, a client's hash is
  // a plain one (for tests).
  explicit HeatStudies(std::filesystem::path dir);
  HeatStudies(std::filesystem::path dir, Caps caps, ClientHash hash = {});
  ~HeatStudies();
  HeatStudies(const HeatStudies&) = delete;
  HeatStudies& operator=(const HeatStudies&) = delete;

  // The request path's check: one relaxed atomic load when nothing runs anywhere; otherwise a
  // compare with each running study's domain, without a lock.
  bool watching(std::string_view domain) const;
  // Takes only the domain's study's lock.
  void record(std::string_view domain, const HeatSample& s);

  // Starts one; refused when one already runs on the domain, or kMaxRunning run on the host.
  std::optional<boost::json::object> start(const std::string& domain, const std::string& tenant, int minutes,
                                           const std::string& why, const std::string& by, int64_t now_ms,
                                           std::string* error);
  // Ends one early, keeping what was gathered; answers the summary row.
  std::optional<boost::json::object> stop(int64_t id, const std::string& by, int64_t now_ms, std::string* error);
  // A running study's live report, or a finished one's from its file.
  std::optional<boost::json::object> get(int64_t id, int64_t now_ms, std::string* error) const;
  // Summary rows, newest first; only one domain's when `domain` isn't empty.
  boost::json::array list(const std::string& domain, int64_t now_ms) const;
  std::optional<std::string> tenant_of(int64_t id) const;

  // Each second: ends studies whose time is up, saves running ones once a minute, promotes busy
  // endpoints. Answers the summary rows of the studies that ended.
  std::vector<boost::json::object> tick(int64_t now_ms, int64_t budget_bytes = 0);
  // The host stopping: every running study ends ("stopped", ended by host_stopping).
  std::vector<boost::json::object> stop_all(int64_t now_ms);
  // Finished reports past 30 days go; then, past `budget_bytes` (0: none), the oldest finished.
  int prune(int64_t now_ms, int64_t budget_bytes);
  int64_t bytes() const;  // every report on disk
  int running() const { return running_.load(std::memory_order_relaxed); }
  std::vector<std::string> recovered() const;  // notes from the constructor
  // Tests only: called in tick between building a running study's save and writing it, where a
  // stop on another thread may end the study.
  void set_before_save_for_test(std::function<void(int64_t id)> f) { before_save_ = std::move(f); }

 private:
  struct Row {
    boost::json::object summary;
    int64_t bytes = 0;
  };
  Study* find(std::string_view domain) const;
  std::optional<boost::json::object> finish(const std::shared_ptr<Study>& st, const std::string& state,
                                            const std::string& ended_by, int64_t ended_ms);
  bool write(int64_t id, const boost::json::object& report, int64_t* bytes);

  std::filesystem::path dir_;
  Caps caps_;
  ClientHash hash_;
  // The running studies, for the request path: read without a lock. A study's memory outlives
  // its slot (`ended_` keeps the small shell of each, its counts freed), so a request that read
  // a slot just before the study ended finds it ended under the study's lock.
  std::array<std::atomic<Study*>, kMaxRunning> slots_{};
  std::atomic<int> running_{0};
  mutable std::mutex mu_;
  std::map<int64_t, std::shared_ptr<Study>> live_;  // running
  std::vector<std::shared_ptr<Study>> ended_;       // shells of the ones that ran since the start
  std::map<int64_t, Row> done_;                     // finished, from their files
  int64_t next_id_ = 1;
  std::vector<std::string> recovered_;
  std::function<void(int64_t)> before_save_;
};

}  // namespace goober::operations
