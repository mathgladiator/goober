#pragma once

// The controls gate (design/machine/047.Controls.md § 5, § 6): applies a domain's compiled
// controls to one request (rate buckets, in-flight counts, body size, closed), and counts what
// every control saw in five-minute buckets in memory, handed to the traffic rollups as they
// close. Everything is in memory: a check is a few map lookups under one lock, never a disk read.

#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "goober/controls/spec.h"
#include "goober/operations/traffic.h"

namespace goober::controls {

inline constexpr int64_t kBucketMs = 5 * 60 * 1000;

// What the gate needs to know about a request.
struct Ask {
  std::string_view method;  // lowercase
  const std::vector<std::string>* path = nullptr;
  std::string_view api;     // box:<name>, content, mcp, identity, or "" when no API control needs it
  std::string client;       // the client's rate key
  // The signed-in account's key, asked only when a `by: user` rate applies; "" counts the client.
  std::function<std::string()> user;
  int64_t body_bytes = 0;
  int64_t now_ms = 0;
};

struct State;

// Held while an allowed request runs: done() counts its latency and status and releases its
// in-flight slots; a ticket dropped without done() releases them too, so no finish path leaks one.
class Ticket {
 public:
  ~Ticket();
  Ticket(const Ticket&) = delete;
  Ticket& operator=(const Ticket&) = delete;
  void done(int status, int64_t us, int64_t now_ms);
  bool finished() const { return done_; }

 private:
  friend class Gate;
  Ticket() = default;
  void release(bool count, int status, int64_t us, int64_t now_ms);
  std::shared_ptr<State> state_;
  std::string domain_, tenant_, client_;
  std::vector<std::string> ids_;                                   // every monitored id the request met
  std::vector<std::pair<std::string, std::string>> per_client_;   // (scope id, client) slots held
  std::vector<std::pair<std::string, int64_t>> observed_timeout_;  // (id, ms): would-refuse when slower
  std::vector<std::string> would_counted_;                         // ids already counted as would-refuse
  bool done_ = false;
};

struct Verdict {
  bool matched = false;  // some control applied
  bool refused = false;
  Reason reason = Reason::kRate;
  std::string control;        // the refusing control's id
  int status = 0;             // 429, 503, 413, or closed's status
  int64_t retry_after_s = -1;  // -1: none
  std::string message;        // closed's message
  int64_t timeout_ms = 0;     // an enforced timeout-ms for the box call, or 0
  std::string id;             // the most specific control the request met
  std::vector<std::pair<std::string, Reason>> would;  // (observing id, reason)
  std::shared_ptr<Ticket> ticket;  // when allowed
};

class Gate {
 public:
  // Names the busiest client in a stored row without naming it: a keyed hash (the host's
  // address hash for logs). The default is a plain truncated SHA-256.
  using Hasher = std::function<std::string(const std::string&)>;
  explicit Gate(Hasher hash = {});
  ~Gate();
  Gate(const Gate&) = delete;
  Gate& operator=(const Gate&) = delete;

  // The compiled controls for a domain (null or empty: none). Takes effect for the next request.
  void put(const std::string& domain, std::shared_ptr<const DomainControls> controls);
  void clear();
  std::shared_ptr<const DomainControls> get(std::string_view domain) const;
  bool any() const;

  Verdict check(const DomainControls& d, const Ask& ask);

  // Rows of buckets that have ended (before now_ms's five minutes), taken: the host hands them
  // to the traffic rollups. take_all also takes the open ones (at shutdown).
  std::vector<operations::ControlRow> take_closed(int64_t now_ms);
  std::vector<operations::ControlRow> take_all();
  // The rows not yet taken, for a domain: a report adds them to what is stored.
  std::vector<operations::ControlRow> pending_rows(const std::string& domain) const;
  int64_t in_flight(const std::string& domain, const std::string& id) const;
  size_t rate_buckets() const;

 private:
  std::shared_ptr<State> state_;
  mutable std::shared_mutex map_mu_;
  std::map<std::string, std::shared_ptr<const DomainControls>, std::less<>> domains_;
};

// The distinct clients a sketch of 64 hex digits holds, estimated (linear counting over 256
// bits); kSketchSaturated when every bit is set.
inline constexpr int64_t kSketchSaturated = 1000;
int64_t sketch_count(const std::string& hex);
// A sketch with one client added.
std::string sketch_add(const std::string& hex, std::string_view client);

}  // namespace goober::controls
