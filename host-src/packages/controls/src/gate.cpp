#include "goober/controls/gate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <set>
#include <unordered_map>

#include "goober/base/hash.h"

namespace goober::controls {

namespace {

uint64_t fnv1a(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

using Bits = std::array<uint64_t, 4>;

Bits bits_of(const std::string& hex) {
  Bits b{};
  if (hex.size() != 64) return b;
  for (size_t i = 0; i < 64; ++i) {
    char c = hex[i];
    uint64_t n = uint64_t(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0') & 15;
    b[i / 16] |= n << (4 * (15 - i % 16));
  }
  return b;
}

std::string hex_of(const Bits& b) {
  if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) return "";
  std::string out(64, '0');
  for (size_t i = 0; i < 64; ++i) out[i] = "0123456789abcdef"[(b[i / 16] >> (4 * (15 - i % 16))) & 15];
  return out;
}

void add_bit(Bits& b, std::string_view client) {
  uint64_t bit = fnv1a(client) & 255;
  b[bit / 64] |= uint64_t(1) << (bit % 64);
}

constexpr size_t kTopTracked = 1024;

}  // namespace

int64_t sketch_count(const std::string& hex) {
  Bits b = bits_of(hex);
  int set = 0;
  for (auto w : b) set += __builtin_popcountll(w);
  if (set == 0) return 0;
  if (set == 256) return kSketchSaturated;
  double zero = double(256 - set) / 256.0;
  return std::min<int64_t>(kSketchSaturated, int64_t(std::llround(-256.0 * std::log(zero))));
}

std::string sketch_add(const std::string& hex, std::string_view client) {
  Bits b = bits_of(hex);
  add_bit(b, client);
  return hex_of(b);
}

struct State {
  struct Side {
    int64_t n = 0;
    Bits bits{};
    std::unordered_map<std::string, int64_t> by_client;
    void add(const std::string& client) {
      ++n;
      add_bit(bits, client);
      auto it = by_client.find(client);
      if (it != by_client.end()) ++it->second;
      else if (by_client.size() < kTopTracked) by_client.emplace(client, 1);
    }
  };
  struct Acc {
    int64_t t = -1;
    std::string tenant, domain, id;
    int64_t matched = 0, allowed = 0, rate = 0, concurrent = 0, body = 0, closed = 0, n5 = 0, peak = 0;
    Side refused, would;
    operations::Histogram hist;
  };
  struct Live {
    int64_t in_flight = 0;
    std::unordered_map<std::string, int64_t> per_client;
  };
  struct Bucket {
    double tokens = 0;
    int64_t last_ms = 0;
  };

  Gate::Hasher hash;
  mutable std::mutex mu;
  std::map<std::string, Acc> accs;  // "domain\nid"
  std::vector<operations::ControlRow> closed;
  std::unordered_map<std::string, Live> live;  // "domain\nid"
  std::unordered_map<std::string, Bucket> buckets;
  uint64_t takes = 0;

  static std::string key(const std::string& domain, const std::string& id) { return domain + "\n" + id; }

  operations::ControlRow row(const Acc& a) const {
    operations::ControlRow r;
    r.t = a.t;
    r.tenant = a.tenant;
    r.domain = a.domain;
    r.control = a.id;
    r.matched = a.matched;
    r.allowed = a.allowed;
    r.rate = a.rate;
    r.concurrent = a.concurrent;
    r.body = a.body;
    r.closed = a.closed;
    r.would = a.would.n;
    r.n5 = a.n5;
    r.peak = a.peak;
    r.clients = hex_of(a.refused.bits);
    r.would_clients = hex_of(a.would.bits);
    auto top = [&](const Side& s, std::string* who, int64_t* n) {
      const std::pair<const std::string, int64_t>* best = nullptr;
      for (const auto& kv : s.by_client) {
        if (!best || kv.second > best->second || (kv.second == best->second && kv.first < best->first)) best = &kv;
      }
      if (!best) return;
      *who = hash ? hash(best->first) : base::sha256_hex("goober-control-client\n" + best->first).substr(0, 16);
      *n = best->second;
    };
    top(a.refused, &r.top_client, &r.top_n);
    top(a.would, &r.would_top, &r.would_top_n);
    r.hist = a.hist;
    return r;
  }

  // The accumulator of (domain, id) for now's five minutes; an ended one becomes a row.
  Acc& acc(const std::string& domain, const std::string& tenant, const std::string& id, int64_t now_ms) {
    int64_t t = now_ms / kBucketMs * kBucketMs;
    Acc& a = accs[key(domain, id)];
    if (a.t != t) {
      if (a.t >= 0 && a.t < t) closed.push_back(row(a));
      // A clock that steps back keeps counting in the newer bucket.
      if (a.t < t) {
        a = Acc{};
        a.t = t;
      }
    }
    a.tenant = tenant;
    a.domain = domain;
    a.id = id;
    return a;
  }

  // A token bucket: 0 when a token was taken, else the seconds until one is there.
  int64_t take(const std::string& k, const Rate& r, int64_t now_ms) {
    if (++takes % 4096 == 0) {
      for (auto it = buckets.begin(); it != buckets.end();) {
        it = now_ms - it->second.last_ms > 15 * 60 * 1000 ? buckets.erase(it) : std::next(it);
      }
    }
    auto [it, fresh] = buckets.try_emplace(k, Bucket{r.burst, now_ms});
    Bucket& b = it->second;
    if (!fresh) {
      double elapsed = double(std::max<int64_t>(0, now_ms - b.last_ms));
      b.tokens = std::min(r.burst, b.tokens + elapsed * r.per_minute / 60000.0);
      b.last_ms = now_ms;
    }
    if (b.tokens >= 1) {
      b.tokens -= 1;
      return 0;
    }
    double wait_ms = (1 - b.tokens) * 60000.0 / r.per_minute;
    return std::max<int64_t>(1, int64_t(std::ceil(wait_ms / 1000.0)));
  }
};

Gate::Gate(Hasher hash) : state_(std::make_shared<State>()) { state_->hash = std::move(hash); }
Gate::~Gate() = default;

void Gate::put(const std::string& domain, std::shared_ptr<const DomainControls> controls) {
  std::unique_lock lock(map_mu_);
  if (!controls || controls->empty()) {
    domains_.erase(domain);
  } else {
    domains_[domain] = std::move(controls);
  }
}

void Gate::clear() {
  std::unique_lock lock(map_mu_);
  domains_.clear();
}

std::shared_ptr<const DomainControls> Gate::get(std::string_view domain) const {
  std::shared_lock lock(map_mu_);
  auto it = domains_.find(domain);
  return it == domains_.end() ? nullptr : it->second;
}

bool Gate::any() const {
  std::shared_lock lock(map_mu_);
  return !domains_.empty();
}

Verdict Gate::check(const DomainControls& d, const Ask& ask) {
  static const std::vector<std::string> kRoot;
  Verdict v;
  auto chain = d.chain(ask.method, ask.path ? *ask.path : kRoot, ask.api, ask.box);
  std::vector<std::string> ids;
  for (const Control* c : chain) {
    if (!c) continue;
    const auto& id = monitored_id(*c);
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  }
  if (ids.empty()) return v;
  v.matched = true;
  v.id = ids.back();

  Effective eff[kProperties.size()];
  for (size_t i = 0; i < kProperties.size(); ++i) eff[i] = effective(chain, kProperties[i]);
  const auto& rate = eff[size_t(Property::kRate)];
  const auto& conc = eff[size_t(Property::kConcurrent)];
  const auto& body = eff[size_t(Property::kBodyMaxBytes)];
  const auto& timeout = eff[size_t(Property::kTimeoutMs)];
  const auto& closed = eff[size_t(Property::kClosed)];
  const auto& fuel = eff[size_t(Property::kFuel)];

  std::string user_key;
  bool user_asked = false;
  auto rate_key = [&](const Control& c, const Rate& r, bool observing) {
    std::string k = d.domain + "\n" + monitored_id(c) + (observing ? "\no\n" : "\ne\n");
    switch (r.by) {
      case By::kAll: return k + "*";
      case By::kUser:
        if (!user_asked) {
          user_asked = true;
          if (ask.user) user_key = ask.user();
        }
        if (!user_key.empty()) return k + "u:" + user_key;
        return k + "c:" + ask.client;
      case By::kClient: return k + "c:" + ask.client;
    }
    return k + "c:" + ask.client;
  };

  auto ticket = std::shared_ptr<Ticket>(new Ticket());
  ticket->state_ = state_;
  ticket->domain_ = d.domain;
  ticket->tenant_ = d.tenant;
  ticket->client_ = ask.client;

  std::lock_guard lock(state_->mu);
  auto& s = *state_;
  for (const auto& id : ids) ++s.acc(d.domain, d.tenant, id, ask.now_ms).matched;
  auto live = [&](const Control& c) -> State::Live& { return s.live[State::key(d.domain, monitored_id(c))]; };

  // Observations first: they refuse nothing, and count what they would have refused.
  auto would = [&](const Control& c, Reason r) {
    const auto& id = monitored_id(c);
    v.would.emplace_back(id, r);
    if (std::find(ticket->would_counted_.begin(), ticket->would_counted_.end(), id) != ticket->would_counted_.end()) return;
    ticket->would_counted_.push_back(id);
    s.acc(d.domain, d.tenant, id, ask.now_ms).would.add(ask.client);
  };
  if (const Control* c = closed.observed; c && c->props.closed.on()) would(*c, Reason::kClosed);
  if (const Control* c = body.observed; c && c->props.body_max_bytes.on() && ask.body_bytes > c->props.body_max_bytes.value) {
    would(*c, Reason::kBody);
  }
  if (const Control* c = rate.observed; c && c->props.rate.on()) {
    if (s.take(rate_key(*c, c->props.rate.value, true), c->props.rate.value, ask.now_ms)) would(*c, Reason::kRate);
  }
  if (const Control* c = conc.observed; c && c->props.concurrent.on()) {
    const auto& cv = c->props.concurrent.value;
    auto& l = live(*c);
    bool over = cv.max && l.in_flight >= cv.max;
    if (cv.per_client) {
      auto it = l.per_client.find(ask.client);
      over |= it != l.per_client.end() && it->second >= cv.per_client;
    }
    if (over) would(*c, Reason::kConcurrent);
  }
  if (const Control* c = timeout.observed; c && c->props.timeout_ms.on()) {
    ticket->observed_timeout_.emplace_back(monitored_id(*c), c->props.timeout_ms.value);
  }

  // Enforcement: closed, then the body, then the rate, then concurrency (which holds a slot).
  auto refuse = [&](const Control& c, Reason r, int status, int64_t retry) {
    v.refused = true;
    v.reason = r;
    v.control = monitored_id(c);
    v.status = status;
    v.retry_after_s = retry;
    auto& a = s.acc(d.domain, d.tenant, v.control, ask.now_ms);
    switch (r) {
      case Reason::kRate: ++a.rate; break;
      case Reason::kConcurrent: ++a.concurrent; break;
      case Reason::kBody: ++a.body; break;
      case Reason::kClosed: ++a.closed; break;
      case Reason::kTimeout: break;
    }
    a.refused.add(ask.client);
    ticket->would_counted_.clear();
    ticket->state_.reset();  // nothing held: its destructor releases nothing
    return v;
  };
  if (const Control* c = closed.enforced; c && c->props.closed.on()) {
    const auto& cl = c->props.closed.value;
    v.message = cl.message;
    return refuse(*c, Reason::kClosed, cl.status, cl.retry_after_s);
  }
  if (const Control* c = body.enforced; c && c->props.body_max_bytes.on() && ask.body_bytes > c->props.body_max_bytes.value) {
    return refuse(*c, Reason::kBody, 413, -1);
  }
  if (const Control* c = rate.enforced; c && c->props.rate.on()) {
    if (int64_t wait = s.take(rate_key(*c, c->props.rate.value, false), c->props.rate.value, ask.now_ms)) {
      return refuse(*c, Reason::kRate, 429, wait);
    }
  }
  if (const Control* c = conc.enforced; c && c->props.concurrent.on()) {
    const auto& cv = c->props.concurrent.value;
    auto& l = live(*c);
    if (cv.max && l.in_flight >= cv.max) return refuse(*c, Reason::kConcurrent, 503, 1);
    if (cv.per_client) {
      auto it = l.per_client.find(ask.client);
      if (it != l.per_client.end() && it->second >= cv.per_client) return refuse(*c, Reason::kConcurrent, 503, 1);
    }
  }
  if (const Control* c = timeout.enforced; c && c->props.timeout_ms.on()) v.timeout_ms = c->props.timeout_ms.value;
  if (const Control* c = fuel.enforced) v.fuel = c->props.fuel.off ? 0 : c->props.fuel.value;

  // Allowed: every control it met holds it in flight until it finishes.
  for (const auto& id : ids) {
    auto& a = s.acc(d.domain, d.tenant, id, ask.now_ms);
    ++a.allowed;
    auto& l = s.live[State::key(d.domain, id)];
    ++l.in_flight;
    a.peak = std::max(a.peak, l.in_flight);
  }
  for (const Control* c : {conc.enforced, conc.observed}) {
    if (!c || !c->props.concurrent.on() || !c->props.concurrent.value.per_client) continue;
    const auto& id = monitored_id(*c);
    bool held = false;
    for (const auto& pc : ticket->per_client_) held |= pc.first == id;
    if (held) continue;
    ++live(*c).per_client[ask.client];
    ticket->per_client_.emplace_back(id, ask.client);
  }
  ticket->ids_ = ids;
  v.ticket = std::move(ticket);
  return v;
}

void Ticket::release(bool count, int status, int64_t us, int64_t now_ms) {
  if (done_ || !state_) return;
  done_ = true;
  auto& s = *state_;
  std::lock_guard lock(s.mu);
  for (const auto& id : ids_) {
    auto it = s.live.find(State::key(domain_, id));
    if (it != s.live.end() && it->second.in_flight > 0) --it->second.in_flight;
    if (!count) continue;
    auto& a = s.acc(domain_, tenant_, id, now_ms);
    a.hist.add(us);
    if (status >= 500) ++a.n5;
  }
  for (const auto& [id, client] : per_client_) {
    auto it = s.live.find(State::key(domain_, id));
    if (it == s.live.end()) continue;
    auto pc = it->second.per_client.find(client);
    if (pc != it->second.per_client.end() && --pc->second <= 0) it->second.per_client.erase(pc);
  }
  if (!count) return;
  for (const auto& [id, ms] : observed_timeout_) {
    if (us <= ms * 1000) continue;
    if (std::find(would_counted_.begin(), would_counted_.end(), id) != would_counted_.end()) continue;
    s.acc(domain_, tenant_, id, now_ms).would.add(client_);
  }
}

void Ticket::done(int status, int64_t us, int64_t now_ms) { release(true, status, us, now_ms); }

Ticket::~Ticket() {
  // A request that never finished (a dropped handler) still gives its slots back.
  if (!done_ && state_) release(false, 0, 0, 0);
}

std::vector<operations::ControlRow> Gate::take_closed(int64_t now_ms) {
  int64_t t = now_ms / kBucketMs * kBucketMs;
  std::lock_guard lock(state_->mu);
  auto& s = *state_;
  for (auto it = s.accs.begin(); it != s.accs.end();) {
    if (it->second.t >= 0 && it->second.t < t) {
      s.closed.push_back(s.row(it->second));
      it = s.accs.erase(it);
    } else {
      ++it;
    }
  }
  // Idle in-flight entries hold nothing.
  for (auto it = s.live.begin(); it != s.live.end();) {
    it = it->second.in_flight == 0 && it->second.per_client.empty() ? s.live.erase(it) : std::next(it);
  }
  std::vector<operations::ControlRow> out;
  out.swap(s.closed);
  return out;
}

std::vector<operations::ControlRow> Gate::take_all() {
  std::lock_guard lock(state_->mu);
  auto& s = *state_;
  for (const auto& [k, a] : s.accs) {
    if (a.t >= 0) s.closed.push_back(s.row(a));
  }
  s.accs.clear();
  std::vector<operations::ControlRow> out;
  out.swap(s.closed);
  return out;
}

std::vector<operations::ControlRow> Gate::pending_rows(const std::string& domain) const {
  std::lock_guard lock(state_->mu);
  const auto& s = *state_;
  std::vector<operations::ControlRow> out;
  for (const auto& r : s.closed) {
    if (r.domain == domain) out.push_back(r);
  }
  for (const auto& [k, a] : s.accs) {
    if (a.domain == domain && a.t >= 0) out.push_back(s.row(a));
  }
  return out;
}

int64_t Gate::in_flight(const std::string& domain, const std::string& id) const {
  std::lock_guard lock(state_->mu);
  auto it = state_->live.find(State::key(domain, id));
  return it == state_->live.end() ? 0 : it->second.in_flight;
}

size_t Gate::rate_buckets() const {
  std::lock_guard lock(state_->mu);
  return state_->buckets.size();
}

}  // namespace goober::controls
