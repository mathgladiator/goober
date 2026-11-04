#include "goober/controls/queue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

#include "goober/controls/gate.h"

namespace goober::controls {

namespace {

constexpr int64_t kMinuteMs = 60 * 1000;
constexpr int64_t kHourMs = 60 * kMinuteMs;
constexpr int64_t kIdleBucketMs = 15 * kMinuteMs;

int64_t system_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// A call is cheap only when its one method is exactly one of these, written without escapes
// (047 § 14): anything else, a method or key with a backslash, or a method named twice, counts
// full, so no spelling the JSON parser reads as tools/call is charged as a ping.
bool cheap_method(std::string_view m) {
  return m == "initialize" || m == "ping" || m == "tools/list" || m == "resources/list" ||
         m == "resources/templates/list" || m == "prompts/list" || m.starts_with("notifications/");
}

double ratio(int64_t part, int64_t whole) { return whole > 0 ? double(part) / double(whole) : 0.0; }

std::string pct(double share) {
  double p = share * 100;
  char buf[32];
  if (p > 0 && p < 10) std::snprintf(buf, sizeof buf, "%.1f%%", p);
  else std::snprintf(buf, sizeof buf, "%.0f%%", p);
  return buf;
}

std::string calls_text(int64_t n) { return std::to_string(n) + (n == 1 ? " call" : " calls"); }

}  // namespace

std::string_view queue_kind_name(QueueKind k) { return k == QueueKind::kOperator ? "operator" : "mcp"; }

const QueueLimits& limits_of(QueueKind k) { return k == QueueKind::kOperator ? kOperatorLimits : kMcpLimits; }

int queue_running_for(int pool_threads) { return std::max(1, pool_threads / 2); }

int total_running_for(int pool_threads) {
  if (pool_threads <= 1) return 1;
  return std::max(1, pool_threads - std::max(1, pool_threads / 4));
}

RpcCalls count_rpc_calls(std::string_view b) {
  RpcCalls out;
  size_t i = 0, n = b.size();
  while (i < n && space(b[i])) ++i;
  out.batch = i < n && b[i] == '[';
  const int element = out.batch ? 2 : 1;  // the depth just inside a request object
  int depth = 0, counted = 0;
  bool in_element = false;
  std::string_view key, method;
  int methods = 0;       // how often the element names a method
  bool escaped = false;  // a key or the method written with a backslash
  auto counts = [&] { return (out.batch && depth == 1); };  // a bare value in a batch: no request
  while (i < n) {
    char c = b[i];
    if (space(c) || c == ':' || c == ',') {
      ++i;
      continue;
    }
    if (c == '"') {
      size_t start = ++i;
      while (i < n && b[i] != '"') i += b[i] == '\\' ? 2 : 1;
      if (i >= n) break;  // unterminated: what was counted stands
      std::string_view s = b.substr(start, i - start);
      ++i;
      size_t j = i;
      while (j < n && space(b[j])) ++j;
      bool is_key = j < n && b[j] == ':';
      if (counts()) {
        ++out.cheap;
        ++counted;
      } else if (depth == element && in_element) {
        if (is_key) {
          key = s;
          if (s.find('\\') != std::string_view::npos) escaped = true;
        } else if (key == "method") {
          method = s;
          ++methods;
          if (s.find('\\') != std::string_view::npos) escaped = true;
        } else if (key == "id" && !out.batch && s.size() <= 128 &&
                   std::none_of(s.begin(), s.end(), [](char x) { return static_cast<unsigned char>(x) < 0x20; })) {
          out.id = "\"" + std::string(s) + "\"";
        }
      }
      continue;
    }
    if (c == '{' || c == '[') {
      if (c == '[' && counts()) {
        ++out.cheap;  // a nested array in a batch: no request
        ++counted;
      }
      ++depth;
      if (c == '{' && depth == element) {
        in_element = true;
        method = {};
        key = {};
        methods = 0;
        escaped = false;
      }
      ++i;
      continue;
    }
    if (c == '}' || c == ']') {
      if (c == '}' && depth == element && in_element) {
        bool cheap = !escaped && (methods == 0 || (methods == 1 && cheap_method(method)));
        if (cheap) ++out.cheap;
        else ++out.full;
        ++counted;
        in_element = false;
      }
      --depth;
      ++i;
      if (depth <= 0) break;
      continue;
    }
    // A number or a literal.
    size_t start = i;
    while (i < n && !space(b[i]) && b[i] != ',' && b[i] != '}' && b[i] != ']' && b[i] != ':' && b[i] != '"') ++i;
    if (i == start) {
      ++i;
      continue;
    }
    std::string_view tok = b.substr(start, i - start);
    if (counts()) {
      ++out.cheap;
      ++counted;
    } else if (depth == element && in_element && key == "id" && !out.batch && tok.size() <= 32 &&
               tok.find_first_not_of("0123456789-+.eE") == std::string_view::npos) {
      out.id = std::string(tok);
    }
  }
  if (counted == 0) {
    out.full = 0;
    out.cheap = 1;
  }
  return out;
}

// --- Slot ---

Slot::~Slot() { release(); }

void Slot::release() {
  if (released_.exchange(true)) return;
  if (auto q = queue_.lock()) q->release(kind_, scope_, agent_, cls_);
}

void McpQueue::Pending::run() {
  for (auto& f : calls) f();
  calls.clear();
}

// --- McpQueue ---

std::shared_ptr<McpQueue> McpQueue::make(int pool_threads, Clock clock) {
  return std::shared_ptr<McpQueue>(new McpQueue(pool_threads, std::move(clock)));
}

McpQueue::McpQueue(int pool_threads, Clock clock)
    : clock_(clock ? std::move(clock) : Clock(system_now_ms)),
      queue_running_(queue_running_for(pool_threads)),
      total_running_(total_running_for(pool_threads)) {}

operations::McpQueueRow& McpQueue::row(QueueKind kind, const std::string& scope, const std::string& tenant,
                                       const std::string& cls, int64_t now_ms) {
  int64_t t = now_ms / kBucketMs * kBucketMs;
  auto& r = rows_[{t, int(kind), scope, cls}];
  if (r.queue.empty()) {
    r.t = t;
    r.queue = std::string(queue_kind_name(kind));
    r.domain = scope;
    r.cls = cls;
  }
  if (!tenant.empty()) r.tenant = tenant;
  return r;
}

McpQueue::Minute& McpQueue::minute(int64_t now_ms) {
  int64_t m = now_ms / kMinuteMs;
  auto& s = minutes_[size_t(m % int64_t(minutes_.size()))];
  if (s.minute != m) {
    s = Minute{};
    s.minute = m;
  }
  return s;
}

void McpQueue::note_peaks(QueueKind kind, const std::string& scope, const std::string& tenant, const std::string& cls,
                          const Queue& q, int64_t now_ms) {
  auto it = q.by_class.find(cls);
  if (it == q.by_class.end()) return;
  auto& r = row(kind, scope, tenant, cls, now_ms);
  r.peak_running = std::max<int64_t>(r.peak_running, it->second.first);
  r.peak_waiting = std::max<int64_t>(r.peak_waiting, it->second.second);
}

Admission McpQueue::admit(Call call) {
  int64_t now = clock_();
  const QueueLimits& L = limits_of(call.kind);
  std::shared_ptr<Slot> slot;
  {
    std::lock_guard lock(mu_);
    int64_t calls = call.calls.calls();
    auto& r = row(call.kind, call.scope, call.tenant, call.cls, now);
    r.offered += calls;
    minute(now).offered += calls;
    if (stopping_) {
      r.refused_full += calls;
      minute(now).refused += calls;
      return {Outcome::kFull, 1};
    }
    // The strict rate, before anything is queued: no control loosens it.
    if (++admits_ % 4096 == 0) {
      for (auto it = buckets_.begin(); it != buckets_.end();) {
        it = now - it->second.last_ms > kIdleBucketMs ? buckets_.erase(it) : std::next(it);
      }
    }
    auto [bit, fresh] = buckets_.try_emplace({int(call.kind), call.agent}, Bucket{L.burst, now});
    Bucket& b = bit->second;
    if (!fresh) {
      b.tokens = std::min(L.burst, b.tokens + double(std::max<int64_t>(0, now - b.last_ms)) * L.per_minute / 60000.0);
      b.last_ms = now;
    }
    double cost = call.calls.cost(L.cheap_cost);
    if (b.tokens + 1e-9 < cost) {
      r.refused_rate += calls;
      minute(now).refused += calls;
      double wait_ms = (cost - b.tokens) * 60000.0 / L.per_minute;
      return {Outcome::kRate, std::max<int64_t>(1, int64_t(std::ceil(wait_ms / 1000.0)))};
    }
    b.tokens -= cost;

    Key key{int(call.kind), call.scope};
    Queue& q = queues_[key];
    if (!call.tenant.empty()) q.tenant = call.tenant;
    Agent& a = q.agents[call.agent];
    a.cls = call.cls;
    auto& cls = q.by_class[call.cls];
    if (a.waiting.empty() && a.running < L.agent_running && q.running < queue_running_ && running_ < total_running_) {
      ++a.running;
      ++q.running;
      ++running_;
      ++cls.first;
      r.ran += calls;
      q.last_served = call.agent;
      last_queue_ = key;
      note_peaks(call.kind, call.scope, call.tenant, call.cls, q, now);
      slot = std::shared_ptr<Slot>(new Slot());
      slot->queue_ = weak_from_this();
      slot->kind_ = call.kind;
      slot->scope_ = call.scope;
      slot->agent_ = call.agent;
      slot->cls_ = call.cls;
    } else if (int(a.waiting.size()) >= L.agent_waiting || a.waiting_bytes + call.bytes > L.agent_waiting_bytes ||
               q.waiting >= L.queue_waiting || waiting_ >= kTotalWaiting || waiting_bytes_ + call.bytes > kTotalWaitingBytes) {
      r.refused_full += calls;
      minute(now).refused += calls;
      if (a.running == 0 && a.waiting.empty()) q.agents.erase(call.agent);
      if (q.agents.empty()) queues_.erase(key);
      return {Outcome::kFull, 1};
    } else {
      a.waiting_bytes += call.bytes;
      waiting_bytes_ += call.bytes;
      ++q.waiting;
      ++waiting_;
      ++cls.second;
      r.queued += calls;
      minute(now).queued += calls;
      std::string scope = call.scope, tenant = call.tenant, c = call.cls;
      QueueKind kind = call.kind;
      a.waiting.push_back(Waiting{std::move(call), now});
      note_peaks(kind, scope, tenant, c, q, now);
      return {Outcome::kQueued, 0};
    }
  }
  // Outside the lock: run may post to the pool, or even finish at once and release.
  call.run(std::move(slot));
  return {Outcome::kRan, 0};
}

void McpQueue::leave(Queue& q, const std::string& agent_key, Agent& a, Waiting& w) {
  (void)agent_key;
  a.waiting_bytes -= w.call.bytes;
  waiting_bytes_ -= w.call.bytes;
  --q.waiting;
  --waiting_;
  auto it = q.by_class.find(w.call.cls);
  if (it != q.by_class.end()) --it->second.second;
}

void McpQueue::pump(int64_t now, Pending& out) {
  while (running_ < total_running_ && waiting_ > 0) {
    // Round-robin across queues, then across the agents of the one picked, each starting after
    // the one served last: no queue starves another, and no agent another.
    std::optional<std::map<Key, Queue>::iterator> pick_q;
    std::map<std::string, Agent>::iterator pick_a;
    auto qi = queues_.upper_bound(last_queue_);
    for (size_t i = 0; i < queues_.size() && !pick_q; ++i, ++qi) {
      if (qi == queues_.end()) qi = queues_.begin();
      Queue& q = qi->second;
      if (q.waiting == 0 || q.running >= queue_running_) continue;
      const QueueLimits& L = limits_of(QueueKind(qi->first.first));
      auto ai = q.agents.upper_bound(q.last_served);
      for (size_t j = 0; j < q.agents.size(); ++j, ++ai) {
        if (ai == q.agents.end()) ai = q.agents.begin();
        if (!ai->second.waiting.empty() && ai->second.running < L.agent_running) {
          pick_q = qi;
          pick_a = ai;
          break;
        }
      }
    }
    if (!pick_q) break;
    auto qit = *pick_q;
    Queue& q = qit->second;
    Agent& a = pick_a->second;
    Waiting w = std::move(a.waiting.front());
    a.waiting.pop_front();
    leave(q, pick_a->first, a, w);
    QueueKind kind = QueueKind(qit->first.first);
    const std::string& scope = qit->first.second;
    int64_t calls = w.call.calls.calls();
    if (w.call.gone && w.call.gone()) {
      // The client went away while it waited: removed, never run.
      row(kind, scope, w.call.tenant, w.call.cls, now).gone += calls;
      minute(now).gone += calls;
      if (w.call.refuse) out.calls.push_back([f = std::move(w.call.refuse)] { f(Refusal::kGone); });
      if (a.running == 0 && a.waiting.empty()) q.agents.erase(pick_a);
      if (q.agents.empty()) queues_.erase(qit);
      continue;
    }
    last_queue_ = qit->first;
    q.last_served = pick_a->first;
    ++a.running;
    ++q.running;
    ++running_;
    ++q.by_class[w.call.cls].first;
    auto& r = row(kind, scope, w.call.tenant, w.call.cls, now);
    r.ran += calls;
    int64_t waited_us = std::max<int64_t>(0, now - w.at_ms) * 1000;
    for (int64_t k = 0; k < calls; ++k) r.waited.add(waited_us);
    minute(now).waited.add(waited_us);
    note_peaks(kind, scope, w.call.tenant, w.call.cls, q, now);
    auto slot = std::shared_ptr<Slot>(new Slot());
    slot->queue_ = weak_from_this();
    slot->kind_ = kind;
    slot->scope_ = scope;
    slot->agent_ = pick_a->first;
    slot->cls_ = w.call.cls;
    out.calls.push_back([f = std::move(w.call.run), slot = std::move(slot)]() mutable { f(std::move(slot)); });
  }
}

void McpQueue::release(QueueKind kind, const std::string& scope, const std::string& agent, const std::string& cls) {
  Pending out;
  {
    std::lock_guard lock(mu_);
    int64_t now = clock_();
    --running_;
    Key key{int(kind), scope};
    auto qit = queues_.find(key);
    if (qit != queues_.end()) {
      Queue& q = qit->second;
      --q.running;
      if (auto c = q.by_class.find(cls); c != q.by_class.end()) --c->second.first;
      if (auto ai = q.agents.find(agent); ai != q.agents.end()) {
        --ai->second.running;
        if (ai->second.running == 0 && ai->second.waiting.empty()) q.agents.erase(ai);
      }
      if (q.agents.empty()) queues_.erase(qit);
    }
    pump(now, out);
  }
  out.run();
}

void McpQueue::sweep() {
  Pending out;
  {
    std::lock_guard lock(mu_);
    int64_t now = clock_();
    for (auto qit = queues_.begin(); qit != queues_.end();) {
      Queue& q = qit->second;
      QueueKind kind = QueueKind(qit->first.first);
      const QueueLimits& L = limits_of(kind);
      for (auto ai = q.agents.begin(); ai != q.agents.end();) {
        Agent& a = ai->second;
        for (auto wi = a.waiting.begin(); wi != a.waiting.end();) {
          bool gone = wi->call.gone && wi->call.gone();
          bool late = now - wi->at_ms > L.max_wait_ms;
          if (!gone && !late) {
            ++wi;
            continue;
          }
          int64_t calls = wi->call.calls.calls();
          auto& r = row(kind, qit->first.second, wi->call.tenant, wi->call.cls, now);
          if (gone) {
            r.gone += calls;
            minute(now).gone += calls;
          } else {
            r.refused_wait += calls;
            minute(now).refused += calls;
          }
          leave(q, ai->first, a, *wi);
          if (wi->call.refuse) {
            out.calls.push_back([f = std::move(wi->call.refuse), gone] { f(gone ? Refusal::kGone : Refusal::kWaited); });
          }
          wi = a.waiting.erase(wi);
        }
        ai = a.running == 0 && a.waiting.empty() ? q.agents.erase(ai) : std::next(ai);
      }
      qit = q.agents.empty() ? queues_.erase(qit) : std::next(qit);
    }
  }
  out.run();
}

void McpQueue::stop() {
  Pending out;
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
    int64_t now = clock_();
    for (auto& [key, q] : queues_) {
      for (auto& [name, a] : q.agents) {
        for (auto& w : a.waiting) {
          leave(q, name, a, w);
          row(QueueKind(key.first), key.second, w.call.tenant, w.call.cls, now).refused_full += w.call.calls.calls();
          if (w.call.refuse) out.calls.push_back([f = std::move(w.call.refuse)] { f(Refusal::kStopping); });
        }
        a.waiting.clear();
      }
    }
  }
  out.run();
}

QueueStats McpQueue::stats() const {
  std::lock_guard lock(mu_);
  size_t agents = 0;
  for (const auto& [k, q] : queues_) agents += q.agents.size();
  return QueueStats{running_, waiting_, waiting_bytes_, agents, buckets_.size(), queue_running_, total_running_};
}

QueueStats McpQueue::stats(QueueKind kind, const std::string& scope) const {
  std::lock_guard lock(mu_);
  QueueStats s{0, 0, 0, 0, buckets_.size(), queue_running_, total_running_};
  auto it = queues_.find(Key{int(kind), scope});
  if (it == queues_.end()) return s;
  s.running = it->second.running;
  s.waiting = it->second.waiting;
  s.agents = it->second.agents.size();
  for (const auto& [n, a] : it->second.agents) s.waiting_bytes += a.waiting_bytes;
  return s;
}

std::vector<operations::McpQueueRow> McpQueue::take_closed() {
  int64_t open = clock_() / kBucketMs * kBucketMs;
  std::lock_guard lock(mu_);
  std::vector<operations::McpQueueRow> out;
  for (auto it = rows_.begin(); it != rows_.end();) {
    if (std::get<0>(it->first) < open) {
      out.push_back(std::move(it->second));
      it = rows_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

std::vector<operations::McpQueueRow> McpQueue::take_all() {
  std::lock_guard lock(mu_);
  std::vector<operations::McpQueueRow> out;
  for (auto& [k, r] : rows_) out.push_back(std::move(r));
  rows_.clear();
  return out;
}

std::vector<operations::McpQueueRow> McpQueue::pending_rows(QueueKind kind, const std::string& domain) const {
  std::lock_guard lock(mu_);
  std::vector<operations::McpQueueRow> out;
  for (const auto& [k, r] : rows_) {
    if (std::get<1>(k) == int(kind) && (domain.empty() || std::get<2>(k) == domain)) out.push_back(r);
  }
  return out;
}

bj::object McpQueue::health() const {
  std::lock_guard lock(mu_);
  int64_t now_min = clock_() / kMinuteMs;
  int64_t offered = 0, queued = 0, refused = 0, gone = 0;
  operations::Histogram waited;
  for (const auto& m : minutes_) {
    if (m.minute < 0 || m.minute <= now_min - int64_t(minutes_.size())) continue;
    offered += m.offered;
    queued += m.queued;
    refused += m.refused;
    gone += m.gone;
    waited.merge(m.waited);
  }
  return bj::object{{"waited_ratio", ratio(queued, offered)},
                    {"refused_ratio", ratio(refused, offered)},
                    {"gone_ratio", ratio(gone, offered)},
                    {"wait_p99_ms", double(waited.percentile(0.99)) / 1000.0},
                    {"busy_ratio", ratio(running_, total_running_)}};
}

// --- the report ---

namespace {

struct Window {
  int64_t span_ms, step_ms;
  static std::optional<Window> of(std::string_view w) {
    if (w == "1h") return Window{kHourMs, kBucketMs};
    if (w == "12h") return Window{12 * kHourMs, 2 * kBucketMs};
    if (w == "7d") return Window{7 * 24 * kHourMs, kHourMs};
    return std::nullopt;
  }
  int64_t to(int64_t now) const { return (now / step_ms + 1) * step_ms; }
  int64_t from(int64_t now) const { return to(now) - span_ms; }
};

bj::object totals_json(const operations::McpQueueRow& r) {
  return bj::object{{"offered", r.offered},
                    {"queued", r.queued},
                    {"ran", r.ran},
                    {"refused", r.refused()},
                    {"refused_rate", r.refused_rate},
                    {"refused_full", r.refused_full},
                    {"refused_wait", r.refused_wait},
                    {"gone", r.gone},
                    {"peak_waiting", r.peak_waiting},
                    {"peak_running", r.peak_running},
                    {"wait_p50_ms", double(r.waited.percentile(0.5)) / 1000.0},
                    {"wait_p99_ms", double(r.waited.percentile(0.99)) / 1000.0},
                    {"refused_ratio", ratio(r.refused(), r.offered)},
                    {"waited_ratio", ratio(r.queued, r.offered - r.refused_rate)}};
}

}  // namespace

bj::object queue_report(QueueKind kind, const std::string& domain, std::vector<operations::McpQueueRow> rows,
                        std::string_view window, int64_t now_ms, int pool_threads) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  int64_t from = w.from(now_ms), to = w.to(now_ms);
  size_t n = size_t(w.span_ms / w.step_ms);
  std::vector<operations::McpQueueRow> points(n);
  operations::McpQueueRow total;
  std::map<std::string, operations::McpQueueRow> by_class;
  for (const auto& r : rows) {
    if (r.t < from || r.t >= to) continue;
    size_t i = size_t((r.t - from) / w.step_ms);
    if (i >= n) continue;
    points[i].merge(r);
    total.merge(r);
    by_class[r.cls].merge(r);
  }
  bj::array pts;
  for (size_t i = 0; i < n; ++i) {
    const auto& p = points[i];
    pts.push_back(bj::object{{"t", from + int64_t(i) * w.step_ms},
                             {"offered", p.offered},
                             {"queued", p.queued},
                             {"ran", p.ran},
                             {"refused", p.refused()},
                             {"refused_rate", p.refused_rate},
                             {"refused_full", p.refused_full},
                             {"refused_wait", p.refused_wait},
                             {"gone", p.gone},
                             {"peak_waiting", p.peak_waiting},
                             {"peak_running", p.peak_running},
                             {"wait_p50_ms", double(p.waited.percentile(0.5)) / 1000.0},
                             {"wait_p99_ms", double(p.waited.percentile(0.99)) / 1000.0}});
  }
  bj::object classes;
  for (const auto& [c, r] : by_class) classes[c] = totals_json(r);
  const QueueLimits& L = limits_of(kind);
  bj::object limits{{"per_minute", L.per_minute},
                    {"burst", L.burst},
                    {"cheap_cost", L.cheap_cost},
                    {"agent_running", L.agent_running},
                    {"agent_waiting", L.agent_waiting},
                    {"queue_running", queue_running_for(pool_threads)},
                    {"total_running", total_running_for(pool_threads)},
                    {"max_wait_ms", L.max_wait_ms}};
  bj::array verdicts;
  if (total.offered == 0) {
    verdicts.emplace_back("no MCP calls in " + std::string(window));
  } else {
    if (total.refused_rate > 0) {
      verdicts.emplace_back("refused " + calls_text(total.refused_rate) + " (" + pct(ratio(total.refused_rate, total.offered)) +
                            ") at the strict rate: an agent sent faster than " + std::to_string(int64_t(L.per_minute)) +
                            " a minute, or more than " + std::to_string(int64_t(L.burst)) + " at once");
    }
    if (total.refused_full > 0) {
      verdicts.emplace_back("refused " + calls_text(total.refused_full) + " with the queue full: an agent front-loaded more than " +
                            std::to_string(L.agent_waiting) + " waiting calls");
    }
    if (total.refused_wait > 0) {
      verdicts.emplace_back(calls_text(total.refused_wait) + " waited past " + std::to_string(L.max_wait_ms / 1000) +
                            " s and failed: calls run slower than they arrive");
    }
    if (total.gone > 0) {
      verdicts.emplace_back(calls_text(total.gone) + " left while waiting (the client disconnected): removed, never run");
    }
    if (total.queued > 0 && total.refused() == 0) {
      verdicts.emplace_back(calls_text(total.queued) + " (" + pct(ratio(total.queued, total.offered)) +
                            ") waited their turn, p99 " + std::to_string(total.waited.percentile(0.99) / 1000) +
                            " ms; none refused");
    }
    if (total.queued == 0 && total.refused() == 0 && total.gone == 0) {
      verdicts.emplace_back("every call ran at once (" + calls_text(total.offered) + ")");
    }
  }
  bj::object out{{"queue", std::string(queue_kind_name(kind))},
                 {"window", std::string(window)},
                 {"step_ms", w.step_ms},
                 {"from_ms", from},
                 {"to_ms", to},
                 {"limits", std::move(limits)},
                 {"points", std::move(pts)},
                 {"totals", totals_json(total)},
                 {"by_class", std::move(classes)},
                 {"verdicts", std::move(verdicts)}};
  if (!domain.empty()) out["domain"] = domain;
  return out;
}

}  // namespace goober::controls
