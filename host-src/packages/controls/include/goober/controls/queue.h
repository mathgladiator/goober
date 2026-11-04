#pragma once

// MCP: a strict rate and a fixed queue (design/machine/047.Controls.md § 14). Agents may behave
// badly, so every MCP call is first counted against its agent's strict rate, then admitted to a
// fixed queue: a few running per agent, a few more per queue, the rest waiting their turn in
// order, shared round-robin across agents, and refused once the queue is full or the wait too
// long. A waiting call is a parked continuation: it holds no worker thread. Everything here is
// in memory and fixed (constants, not settings); the host wires it to its pool (mcp_queue.cpp).

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "goober/json/json.h"
#include "goober/operations/traffic.h"

namespace goober::controls {

// The two queues: a domain's /mcp (one queue per domain), and the operator MCP (one queue).
enum class QueueKind { kMcp = 0, kOperator = 1 };
std::string_view queue_kind_name(QueueKind k);  // mcp, operator

// One kind's fixed limits. Per agent (the bearer's OAuth client and subject, an operator token,
// or, before a token is known good, the client address).
struct QueueLimits {
  double per_minute;            // the strict rate, refilled continuously
  double burst;                 // the bucket's size: calls an agent may front-load at once
  double cheap_cost;            // initialize, tools/list, ping, notifications: a fraction of a call
  int agent_running;            // N: an agent's calls running at once
  int agent_waiting;            // M: an agent's calls waiting, first in first out
  int64_t agent_waiting_bytes;  // the request bodies an agent's waiting calls hold
  int queue_waiting;            // every agent's waiting calls in one queue
  int64_t max_wait_ms;          // W: a call that hasn't started by then fails
};

// /mcp: an agent acting for a person on one site. 120 calls a minute (two a second, steady) is
// several times what a model's turn loop makes, and the burst of 30 lets it front-load a plan
// of calls without being refused.
inline constexpr QueueLimits kMcpLimits{120, 30, 0.2, 2, 32, 16ll << 20, 256, 30'000};
// The operator MCP: Grok and the operator's own tools push whole sites (a page per call, dozens
// at a time), so five times /mcp's rate and four times its burst; waits are longer because a
// push is worth finishing, and an operator's bodies are larger (up to 64 MiB).
inline constexpr QueueLimits kOperatorLimits{600, 120, 0.2, 2, 128, 64ll << 20, 256, 120'000};
const QueueLimits& limits_of(QueueKind k);

// Across every queue: waiting calls and the bodies they hold, so no flood of agents grows the
// host's memory past this.
inline constexpr int kTotalWaiting = 1024;
inline constexpr int64_t kTotalWaitingBytes = 128ll << 20;
// The text every refusal says, and nothing else: never which agent, limit, or bucket.
inline constexpr std::string_view kRefusalText = "rate limit reached";

// K: one queue's running calls, half the worker pool and at least 1; and T: every MCP call
// running, leaving at least a quarter of the pool (and at least one worker) to everything else
// once the pool has two workers.
int queue_running_for(int pool_threads);
int total_running_for(int pool_threads);

// The JSON-RPC calls in a body, counted without building it (the network thread reads it):
// tools/call, resources/read, and prompts/get are full calls; everything else (initialize,
// tools/list, ping, notifications, an element that isn't a request) is cheap. A body that holds
// nothing countable is one cheap call.
struct RpcCalls {
  int full = 0, cheap = 0;
  bool batch = false;
  std::string id;  // a lone request's id as raw JSON (a number, or a string of at most 128 bytes), else ""
  int calls() const { return full + cheap; }
  double cost(double cheap_cost) const { return full + cheap * cheap_cost; }
};
RpcCalls count_rpc_calls(std::string_view body);

// Why a call that was admitted to wait left without running.
enum class Refusal { kFull, kWaited, kGone, kStopping };

class McpQueue;

// Held while a call runs. release() (or dropping the last reference) gives the slot back and
// starts the next waiting calls; so every way out of a call, errors and exceptions included,
// frees what it held.
class Slot {
 public:
  ~Slot();
  Slot(const Slot&) = delete;
  Slot& operator=(const Slot&) = delete;
  void release();
  bool released() const { return released_; }

 private:
  friend class McpQueue;
  Slot() = default;
  std::weak_ptr<McpQueue> queue_;
  QueueKind kind_ = QueueKind::kMcp;
  std::string scope_, agent_, cls_;
  std::atomic<bool> released_{false};
};

struct Call {
  QueueKind kind = QueueKind::kMcp;
  std::string scope;   // the domain for kMcp; "" for kOperator
  std::string tenant;  // for the rows
  std::string agent;   // who the agent is (never stored, never shown)
  std::string cls;     // agent, person, unknown, operator
  RpcCalls calls;      // what the body holds: the cost, and what is counted
  int64_t bytes = 0;   // the body's size, held while waiting
  // Called once, outside the queue's lock, when the call may run (at once, or when a slot frees).
  std::function<void(std::shared_ptr<Slot>)> run;
  // Called once, outside the lock, when a waiting call leaves without running.
  std::function<void(Refusal)> refuse;
  // True once the client is gone: a waiting call is then removed, never run. May be empty.
  std::function<bool()> gone;
};

enum class Outcome { kRan, kQueued, kRate, kFull };
struct Admission {
  Outcome outcome = Outcome::kRan;
  int64_t retry_after_s = 0;  // kRate and kFull
};

struct QueueStats {
  int running = 0, waiting = 0;
  int64_t waiting_bytes = 0;
  size_t agents = 0, rate_buckets = 0;
  int queue_running = 0, total_running = 0;  // K and T as sized for the pool
};

class McpQueue : public std::enable_shared_from_this<McpQueue> {
 public:
  using Clock = std::function<int64_t()>;  // unix milliseconds
  static std::shared_ptr<McpQueue> make(int pool_threads, Clock clock = {});
  McpQueue(const McpQueue&) = delete;
  McpQueue& operator=(const McpQueue&) = delete;

  // Counts the call against its agent's strict rate, then runs it (calls run at once), parks it
  // (run later, or refuse), or refuses it (neither callback is called: the caller answers).
  Admission admit(Call call);
  // Waiting calls past their kind's wait, or whose client is gone, leave (refuse is called).
  void sweep();
  // Refuses every waiting call (kStopping) and every call after; running ones finish.
  void stop();
  QueueStats stats() const;
  QueueStats stats(QueueKind kind, const std::string& scope) const;

  // Rows of buckets that have ended, taken; take_all takes the open ones too (at shutdown).
  std::vector<operations::McpQueueRow> take_closed();
  std::vector<operations::McpQueueRow> take_all();
  // Rows not yet taken, of one queue ("" domain: every domain of kMcp).
  std::vector<operations::McpQueueRow> pending_rows(QueueKind kind, const std::string& domain) const;
  // The last five minutes, host-wide, as ratios and a latency only (for /goober/ready, which is
  // public): {waited_ratio, refused_ratio, gone_ratio, wait_p99_ms, busy_ratio}.
  boost::json::object health() const;

 private:
  friend class Slot;
  McpQueue(int pool_threads, Clock clock);
  struct Waiting {
    Call call;
    int64_t at_ms = 0;
  };
  struct Agent {
    int running = 0;
    std::deque<Waiting> waiting;
    int64_t waiting_bytes = 0;
    std::string cls;
  };
  struct Queue {
    int running = 0, waiting = 0;
    std::map<std::string, Agent> agents;
    std::string last_served;  // round-robin: the agent served last
    std::string tenant;
    std::map<std::string, std::pair<int, int>> by_class;  // cls -> (running, waiting)
  };
  using Key = std::pair<int, std::string>;  // (kind, scope)
  struct Bucket {
    double tokens = 0;
    int64_t last_ms = 0;
  };
  struct Minute {
    int64_t minute = -1;
    int64_t offered = 0, queued = 0, refused = 0, gone = 0;
    operations::Histogram waited;
  };
  struct Pending {
    std::vector<std::function<void()>> calls;
    void run();
  };

  void release(QueueKind kind, const std::string& scope, const std::string& agent, const std::string& cls);
  void pump(int64_t now_ms, Pending& out);
  operations::McpQueueRow& row(QueueKind kind, const std::string& scope, const std::string& tenant, const std::string& cls,
                               int64_t now_ms);
  Minute& minute(int64_t now_ms);
  void note_peaks(QueueKind kind, const std::string& scope, const std::string& tenant, const std::string& cls,
                  const Queue& q, int64_t now_ms);
  void leave(Queue& q, const std::string& agent_key, Agent& a, Waiting& w);

  mutable std::mutex mu_;
  Clock clock_;
  int queue_running_, total_running_;
  int running_ = 0, waiting_ = 0;
  int64_t waiting_bytes_ = 0;
  bool stopping_ = false;
  std::map<Key, Queue> queues_;
  Key last_queue_{-1, ""};
  std::map<std::pair<int, std::string>, Bucket> buckets_;  // (kind, agent)
  int64_t admits_ = 0;
  // (bucket start, kind, domain, cls) -> row; tenant kept in the row.
  std::map<std::tuple<int64_t, int, std::string, std::string>, operations::McpQueueRow> rows_;
  std::array<Minute, 5> minutes_{};
};

// What one queue did over a window: {queue, domain?, window, step_ms, from_ms, to_ms, limits,
// points: [{t, offered, queued, ran, refused, refused_rate, refused_full, refused_wait, gone,
// peak_waiting, peak_running, wait_p50_ms, wait_p99_ms}], totals: {...the same, refused_ratio,
// waited_ratio}, by_class: {cls: totals}, verdicts: [text]}.
boost::json::object queue_report(QueueKind kind, const std::string& domain, std::vector<operations::McpQueueRow> rows,
                                 std::string_view window, int64_t now_ms, int pool_threads);

}  // namespace goober::controls
