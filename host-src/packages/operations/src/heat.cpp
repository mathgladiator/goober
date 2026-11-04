#include "goober/operations/heat.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <system_error>
#include <unordered_map>

#include "goober/base/files.h"
#include "goober/json/json.h"

namespace goober::operations {
namespace bj = boost::json;
namespace fs = std::filesystem;

// ------------------------------------------------------------------ small pieces

std::string endpoint_match(std::string_view method, std::string_view pattern) {
  std::string out;
  for (char c : method) out.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  out.push_back(' ');
  if (pattern.empty()) pattern = "/";
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '{') {
      out.push_back(pattern[i]);
      continue;
    }
    size_t close = pattern.find('}', i);
    if (close == std::string_view::npos) {
      out.append(pattern.substr(i));
      break;
    }
    std::string_view inside = pattern.substr(i + 1, close - i - 1);
    inside = inside.substr(0, inside.find(':'));
    out.push_back('{');
    for (char c : inside) {
      if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    }
    out.push_back('}');
    i = close;
  }
  return out;
}

int CountHistogram::bucket(uint32_t v) {
  if (v < 64) return int(v);
  int e = 31 - __builtin_clz(v);
  if (e > 20) return kBuckets - 1;
  int sub = int((v >> (e - 3)) & 7);
  return 64 + (e - 6) * 8 + sub;
}

uint32_t CountHistogram::upper(int b) {
  if (b < 64) return uint32_t(b);
  int k = b - 64, e = k / 8 + 6, sub = k % 8;
  return (uint32_t(8 + sub + 1) << (e - 3)) - 1;
}

void CountHistogram::add(uint32_t v) {
  ++counts_[bucket(v)];
  max_ = std::max(max_, v);
  ++n_;
}

void CountHistogram::merge(const CountHistogram& o) {
  for (int b = 0; b < kBuckets; ++b) counts_[b] += o.counts_[b];
  max_ = std::max(max_, o.max_);
  n_ += o.n_;
}

uint32_t CountHistogram::percentile(double q) const {
  if (!n_) return 0;
  uint64_t rank = uint64_t(std::ceil(q * double(n_)));
  rank = std::clamp<uint64_t>(rank, 1, n_);
  uint64_t seen = 0;
  for (int b = 0; b < kBuckets; ++b) {
    seen += counts_[b];
    if (seen >= rank) return std::min(upper(b), max_);
  }
  return max_;
}

namespace {

uint64_t mix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

uint64_t hash_of(std::string_view s) { return std::hash<std::string_view>{}(s); }

}  // namespace

void DistinctCounter::add(uint64_t hash) {
  uint64_t h = mix(hash);
  size_t idx = size_t(h >> 56);
  uint64_t w = (h << 8) | (uint64_t(1) << 7);
  uint8_t rho = uint8_t(__builtin_clzll(w) + 1);
  reg_[idx] = std::max(reg_[idx], rho);
}

void DistinctCounter::merge(const DistinctCounter& o) {
  for (size_t i = 0; i < reg_.size(); ++i) reg_[i] = std::max(reg_[i], o.reg_[i]);
}

int64_t DistinctCounter::estimate() const {
  constexpr double m = 256;
  double sum = 0;
  int zeros = 0;
  for (uint8_t r : reg_) {
    sum += std::ldexp(1.0, -int(r));
    zeros += r == 0;
  }
  double e = 0.7213 / (1 + 1.079 / m) * m * m / sum;
  if (e <= 2.5 * m && zeros > 0) e = m * std::log(m / double(zeros));
  return int64_t(std::llround(e));
}

// ------------------------------------------------------------------ suggestions

namespace {

int64_t round_up(int64_t v, int64_t step) { return (v + step - 1) / step * step; }
std::string count_of(int64_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// An override id from the endpoint, as 047 allows (letters, digits, _; not a leading digit; at
// most 64): "POST /cart/{id}" is post_cart_id.
std::string override_id(const std::string& endpoint) {
  std::string id;
  for (char c : endpoint) {
    bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'), word = true;
    if (word) id += c;
    else if (!id.empty() && id.back() != '_') id += '_';
  }
  while (!id.empty() && id.back() == '_') id.pop_back();
  if (id.empty() || (id[0] >= '0' && id[0] <= '9')) id = "e_" + id;
  if (id.size() > 64) id.resize(64);
  while (id.back() == '_') id.pop_back();
  return id;
}

}  // namespace

bj::object suggest_controls(const SuggestInput& in) {
  if (in.api == "other" || in.endpoint.empty() || in.endpoint.front() == '(') {
    return bj::object{{"none", "not an endpoint a control names"}};
  }
  if (in.requests < 20) {
    return bj::object{{"none", "only " + count_of(in.requests, "request", "requests") +
                                   ": too few to suggest from; study longer, or at a busier time"}};
  }
  // One MCP tool is matched as "TOOL /<name>", which names no api: it meets only /mcp's tool calls (047 § 13).
  bool tool = in.api == "mcp";
  std::string match = tool ? "TOOL /" + in.endpoint : in.endpoint;
  bj::object args{{"domain", in.domain}};
  if (!tool) args["api"] = in.api;
  args["endpoint"] = bj::object{{"id", override_id(match)}, {"match", match}};
  bj::object set;
  bj::object reasons;
  // A per-client rate protects against one client; when the endpoint carries much of the
  // domain's heat and its load is spread over many clients, the endpoint's whole rate is the
  // risk, and the rate is by all.
  bool spread = in.heat_share >= 0.25 && in.clients >= 20 && in.top_client_share < 0.10;
  int64_t per_minute = 0;
  std::string by;
  if (spread) {
    by = "all";
    per_minute = std::max<int64_t>(10, 2 * int64_t(in.peak_minute));
    reasons["rate"] = "by all, as this endpoint holds " + std::to_string(int(std::lround(in.heat_share * 100))) +
                      "% of the domain's heat over " + count_of(in.clients, "client", "clients") +
                      ", none above 10% of its requests: per-minute " + std::to_string(per_minute) +
                      " = max(10, 2 x " + std::to_string(in.peak_minute) +
                      ", the endpoint's busiest minute); burst is half that";
  } else {
    by = "client";
    per_minute = std::max<int64_t>(10, 2 * int64_t(in.p99_client_minute));
    reasons["rate"] = "per-minute " + std::to_string(per_minute) + " = max(10, 2 x " +
                      std::to_string(in.p99_client_minute) + ", the p99 of each client's busiest minute over " +
                      count_of(in.clients, "client", "clients") + "); burst is half that";
  }
  int64_t burst = std::max<int64_t>(1, (per_minute + 1) / 2);
  set["rate"] = bj::object{{"per-minute", per_minute}, {"burst", burst}, {"by", by}};
  int64_t cmax = std::max<int64_t>(2, 2 * in.peak_concurrent);
  int64_t cper = std::max<int64_t>(1, 2 * in.peak_client_concurrent);
  set["concurrent"] = bj::object{{"max", cmax}, {"per-client", cper}};
  reasons["concurrent"] = "max " + std::to_string(cmax) + " = max(2, 2 x " + std::to_string(in.peak_concurrent) +
                          ", the most in flight at once); per-client " + std::to_string(cper) + " = max(1, 2 x " +
                          std::to_string(in.peak_client_concurrent) + ", the most one client had in flight)";
  if (in.max_body_bytes > 0) {
    int64_t b = std::max<int64_t>(1024, round_up(2 * in.max_body_bytes, 1024));
    set["body-max-bytes"] = b;
    reasons["body-max-bytes"] = std::to_string(b) + " = max(1024, 2 x " + count_of(in.max_body_bytes, "byte", "bytes") +
                                ", the largest body seen, rounded up to a KiB)";
  }
  if (in.api.starts_with("box:")) {
    int64_t ms = std::max<int64_t>(1000, round_up((2 * in.max_us + 999) / 1000, 100));
    set["timeout-ms"] = ms;
    reasons["timeout-ms"] = std::to_string(ms) + " = max(1000, 2 x " + std::to_string((in.max_us + 999) / 1000) +
                            " ms, the slowest answer, rounded up to 100 ms)";
  }
  args["set"] = std::move(set);
  args["mode"] = "observe";
  args["why"] = "heat study " + std::to_string(in.study_id) + ": observe first, then enforce";
  return bj::object{{"control_set", std::move(args)}, {"reasons", std::move(reasons)}};
}

// ------------------------------------------------------------------ accumulators

namespace {

// An endpoint's key: a hash of its parts, so the request path builds no string. Its strings are
// built once, when the endpoint is first seen. Two endpoints share a key with odds of about one
// in 10^12 for 5,500 of them.
uint64_t endpoint_key(const HeatSample& s) {
  uint64_t h = mix(uint64_t(s.api) + 1);
  h = mix(h ^ hash_of(s.box));
  h = mix(h ^ hash_of(s.method));
  return mix(h ^ hash_of(s.what));
}

std::string api_name(const HeatSample& s) {
  switch (s.api) {
    case HeatApi::kBox:
      return "box:" + std::string(s.box);
    case HeatApi::kMcp:
      return "mcp";
    case HeatApi::kIdentity:
      return "identity";
    case HeatApi::kContent:
      break;
  }
  return "content";
}

std::string endpoint_name(const HeatSample& s) {
  if (s.method.empty()) return std::string(s.what);
  if (s.api == HeatApi::kBox) return endpoint_match(s.method, s.what);
  std::string out;
  for (char c : s.method) out.push_back(char(std::toupper(static_cast<unsigned char>(c))));
  out.push_back(' ');
  out.append(s.what);
  return out;
}

// What every row counts, the whole domain's included.
struct Counts {
  int64_t requests = 0, us = 0, bytes_in = 0, bytes_out = 0, max_in = 0, max_us = 0;
  std::array<int64_t, 5> status{};  // 2xx, 3xx, 4xx (not 429), 5xx, 429
  std::vector<uint32_t> minutes;    // requests per minute since the study began
  Histogram latency;

  void add(const HeatSample& s, int64_t minute) {
    int64_t us_ = std::max<int64_t>(0, s.us);
    ++requests;
    us += us_;
    bytes_in += s.bytes_in;
    bytes_out += s.bytes_out;
    max_in = std::max(max_in, s.bytes_in);
    max_us = std::max(max_us, us_);
    int st = s.status;
    ++status[st == 429 ? 4 : st >= 500 ? 3 : st >= 400 ? 2 : st >= 300 ? 1 : 0];
    if (minutes.size() <= size_t(minute)) minutes.resize(size_t(minute) + 1);
    ++minutes[size_t(minute)];
    latency.add(us_);
  }
  void merge(const Counts& o) {
    requests += o.requests;
    us += o.us;
    bytes_in += o.bytes_in;
    bytes_out += o.bytes_out;
    max_in = std::max(max_in, o.max_in);
    max_us = std::max(max_us, o.max_us);
    for (size_t i = 0; i < status.size(); ++i) status[i] += o.status[i];
    if (minutes.size() < o.minutes.size()) minutes.resize(o.minutes.size());
    for (size_t i = 0; i < o.minutes.size(); ++i) minutes[i] += o.minutes[i];
    latency.merge(o.latency);
  }
  std::pair<uint32_t, int64_t> peak_minute() const {
    uint32_t peak = 0;
    int64_t at = 0;
    for (size_t i = 0; i < minutes.size(); ++i) {
      if (minutes[i] > peak) peak = minutes[i], at = int64_t(i);
    }
    return {peak, at};
  }
  bj::object latency_json() const {
    return bj::object{{"p50", std::min(latency.percentile(0.5), max_us)},
                      {"p99", std::min(latency.percentile(0.99), max_us)},
                      {"max", max_us}};
  }
  bj::object status_json() const {
    return bj::object{{"2xx", status[0]}, {"3xx", status[1]}, {"4xx", status[2]}, {"5xx", status[3]}, {"429", status[4]}};
  }
};

struct ClientState {
  int64_t minute = -1;
  uint32_t in_minute = 0, best = 0, requests = 0;
  uint64_t log_h = 0;  // the keyed hash a report shows, made once
  uint32_t busiest() const { return std::max(best, in_minute); }
};

// Each client's busiest minute, kept per client while the client is active; a client idle past
// the current minute is folded into `done` when the table is full.
struct ClientTable {
  size_t cap = 256;
  std::unordered_map<uint64_t, ClientState> live;
  CountHistogram done;
  uint32_t done_top = 0;
  int64_t overflow = 0;
  int64_t compacted_minute = -1;
  bool evicted = false;
  DistinctCounter distinct;

  void add(uint64_t client, std::string_view key, int64_t minute, const HeatStudies::ClientHash& hash) {
    distinct.add(client);
    auto it = live.find(client);
    if (it == live.end()) {
      if (live.size() >= cap && minute > compacted_minute) {
        compacted_minute = minute;
        for (auto c = live.begin(); c != live.end();) {
          if (c->second.minute < minute) {
            done.add(c->second.busiest());
            done_top = std::max(done_top, c->second.requests);
            evicted = true;
            c = live.erase(c);
          } else {
            ++c;
          }
        }
      }
      if (live.size() >= cap) {
        ++overflow;
        return;
      }
      it = live.emplace(client, ClientState{}).first;
      it->second.log_h = hash(key);
    }
    ClientState& c = it->second;
    ++c.requests;
    if (c.minute >= minute) {
      ++c.in_minute;
    } else {
      c.best = c.busiest();
      c.minute = minute;
      c.in_minute = 1;
    }
  }
  CountHistogram busiest() const {
    CountHistogram h = done;
    for (const auto& [id, c] : live) h.add(c.busiest());
    return h;
  }
  uint32_t top_requests() const {
    uint32_t top = done_top;
    for (const auto& [id, c] : live) top = std::max(top, c.requests);
    return top;
  }
  bool exact() const { return !evicted && overflow == 0; }
  int64_t count() const { return exact() ? int64_t(live.size()) : std::max<int64_t>(int64_t(live.size()), distinct.estimate()); }
};

// Peak concurrency from finished requests: each one's interval is checked against the last
// `cap` finished on the same endpoint, both ways, so the count at every start is exact while no
// more than `cap` overlap.
struct Overlap {
  struct Interval {
    int64_t s = 0, e = 0;
    uint64_t client = 0;
    uint32_t all = 1, same = 1;
  };
  size_t cap = 64;
  std::vector<Interval> ring;
  size_t next = 0;
  uint32_t peak = 0, peak_same = 0;

  void add(int64_t s, int64_t e, uint64_t client) {
    Interval r{s, e, client, 1, 1};
    for (auto& j : ring) {
      if (j.s <= s && s < j.e) {
        ++r.all;
        if (j.client == client) ++r.same;
      }
      if (s <= j.s && j.s < e) {
        peak = std::max(peak, ++j.all);
        if (j.client == client) peak_same = std::max(peak_same, ++j.same);
      }
    }
    peak = std::max(peak, r.all);
    peak_same = std::max(peak_same, r.same);
    if (ring.size() < cap) {
      ring.push_back(r);
    } else {
      ring[next] = r;
      next = (next + 1) % cap;
    }
  }
};

struct Endpoint {
  std::string api, endpoint;
  int64_t tracked_from_ms = 0;
  int64_t prior = 0;  // requests counted in "other" before it was tracked
  Counts n;
  ClientTable clients;
  Overlap overlap;

  Endpoint(std::string a, std::string e, const HeatStudies::Caps& caps) : api(std::move(a)), endpoint(std::move(e)) {
    clients.cap = caps.endpoint_clients;
    overlap.cap = caps.endpoint_ring;
  }

  void add(const HeatSample& s, uint64_t client, int64_t minute, const HeatStudies::ClientHash& hash) {
    n.add(s, minute);
    clients.add(client, s.client, minute, hash);
    overlap.add(s.end_us - std::max<int64_t>(0, s.us), s.end_us, client);
  }

  void fold_into(Endpoint& o) const {
    o.n.merge(n);
    o.clients.done.merge(clients.busiest());
    o.clients.done_top = std::max(o.clients.done_top, clients.top_requests());
    o.clients.distinct.merge(clients.distinct);
    o.clients.overflow += clients.overflow;
    o.clients.evicted = true;
    o.overlap.peak = std::max(o.overlap.peak, overlap.peak);
    o.overlap.peak_same = std::max(o.overlap.peak_same, overlap.peak_same);
  }

  int64_t memory() const {
    return int64_t(sizeof(Endpoint)) + int64_t(n.minutes.capacity()) * 4 + int64_t(clients.live.size()) * 64 +
           int64_t(overlap.ring.capacity()) * int64_t(sizeof(Overlap::Interval)) + int64_t(api.size() + endpoint.size());
  }
};

double round4(double v) { return std::round(v * 10000) / 10000; }

std::string hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016" PRIx64, v);
  return buf;
}

}  // namespace

struct Study {
  mutable std::mutex mu;
  HeatStudies::Caps caps;
  const HeatStudies::ClientHash* hash = nullptr;
  int64_t id = 0;
  std::string domain, tenant, why, by, stopped_by;
  int minutes = 60;
  int64_t started_ms = 0, ends_ms = 0, saved_ms = 0;
  std::atomic<int64_t> file_bytes{0};
  int64_t promoted_minute = -1;
  bool ended = false;
  // Held across each write of the study's file, with `ended` read again under it: a running save
  // built before the end never lands after the final report.
  std::mutex write_mu;
  bj::object final_report;  // between the end and its file being written
  Counts all;               // the whole domain
  std::unordered_map<uint64_t, std::unique_ptr<Endpoint>> endpoints;
  struct Candidate {
    std::string api, endpoint;
    int64_t requests = 0;
  };
  std::unordered_map<uint64_t, Candidate> candidates;
  int64_t promoted = 0;
  std::unique_ptr<Endpoint> other;

  void add(const HeatSample& s) {
    if (ended || s.at_ms < started_ms || s.at_ms >= ends_ms) return;
    int64_t minute = (s.at_ms - started_ms) / 60000;
    uint64_t client = hash_of(s.client);
    all.add(s, minute);
    uint64_t key = endpoint_key(s);
    auto it = endpoints.find(key);
    if (it == endpoints.end()) {
      if (endpoints.size() >= caps.endpoints) {
        if (auto c = candidates.find(key); c != candidates.end()) {
          ++c->second.requests;
        } else if (candidates.size() < caps.candidates) {
          candidates.emplace(key, Candidate{api_name(s), endpoint_name(s), 1});
        }
        other->add(s, client, minute, *hash);
        return;
      }
      auto ep = std::make_unique<Endpoint>(api_name(s), endpoint_name(s), caps);
      ep->tracked_from_ms = started_ms;
      it = endpoints.emplace(key, std::move(ep)).first;
    }
    it->second->add(s, client, minute, *hash);
  }

  // Once a minute: an untracked endpoint busier than the least busy tracked one takes its place;
  // the one it replaces is folded into "other".
  void promote(int64_t now_ms) {
    int64_t minute = (now_ms - started_ms) / 60000;
    if (minute == promoted_minute) return;
    promoted_minute = minute;
    if (candidates.empty() || endpoints.empty()) return;
    std::vector<std::pair<int64_t, uint64_t>> ranked;
    for (const auto& [k, c] : candidates) ranked.emplace_back(c.requests, k);
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    auto seen = [](const Endpoint& e) { return e.n.requests + e.prior; };
    for (size_t i = 0; i < ranked.size() && i < 50; ++i) {
      auto low = endpoints.begin();
      for (auto e = endpoints.begin(); e != endpoints.end(); ++e) {
        if (seen(*e->second) < seen(*low->second)) low = e;
      }
      int64_t low_n = seen(*low->second);
      if (ranked[i].first <= low_n) break;
      low->second->fold_into(*other);
      Candidate back{low->second->api, low->second->endpoint, low_n};
      uint64_t back_key = low->first;
      endpoints.erase(low);
      auto c = candidates.find(ranked[i].second);
      auto ep = std::make_unique<Endpoint>(c->second.api, c->second.endpoint, caps);
      ep->tracked_from_ms = now_ms;
      ep->prior = c->second.requests;
      endpoints.emplace(c->first, std::move(ep));
      candidates.erase(c);
      candidates[back_key] = std::move(back);
      ++promoted;
    }
  }

  // The counts are freed once the report is written; the shell stays for the request path.
  void release() {
    all = Counts{};
    endpoints.clear();
    candidates.clear();
    other.reset();
  }

  int64_t memory() const {
    int64_t m = int64_t(sizeof(Study)) + int64_t(all.minutes.capacity()) * 4 + (other ? other->memory() : 0);
    for (const auto& [k, e] : endpoints) m += e->memory() + 32;
    m += int64_t(candidates.size()) * 96;
    return m;
  }

  const Endpoint* hottest() const {
    const Endpoint* h = nullptr;
    for (const auto& [k, e] : endpoints) {
      if (!h || e->n.us > h->n.us || (e->n.us == h->n.us && e->endpoint < h->endpoint)) h = e.get();
    }
    return h;
  }

  bj::object summary(const std::string& state, const std::string& ended_by, int64_t ended_ms) const {
    bj::object row{{"id", id},          {"domain", domain},        {"tenant", tenant}, {"state", state},
                   {"why", why},        {"by", by},                {"minutes", minutes}, {"started_ms", started_ms},
                   {"ends_ms", ends_ms}, {"requests", all.requests}, {"endpoints", int64_t(endpoints.size())}};
    if (state != "running") {
      row["ended_ms"] = ended_ms;
      row["ended_by"] = ended_by;
    }
    if (!stopped_by.empty()) row["stopped_by"] = stopped_by;
    if (const Endpoint* h = hottest(); h && all.us > 0) {
      row["hottest"] = bj::object{{"endpoint", h->endpoint}, {"api", h->api}, {"heat_share", round4(double(h->n.us) / double(all.us))}};
    }
    return row;
  }

  bj::object endpoint_json(const Endpoint& e, bool detailed) const {
    double heat_share = round4(double(e.n.us) / double(std::max<int64_t>(1, all.us)));
    bj::object o{{"endpoint", e.endpoint},
                 {"api", e.api},
                 {"requests", e.n.requests},
                 {"heat_share", heat_share},
                 {"requests_share", round4(double(e.n.requests) / double(std::max<int64_t>(1, all.requests)))},
                 {"bytes_share", round4(double(e.n.bytes_in + e.n.bytes_out) /
                                        double(std::max<int64_t>(1, all.bytes_in + all.bytes_out)))}};
    if (!detailed) return o;
    auto [peak, peak_at] = e.n.peak_minute();
    auto busiest = e.clients.busiest();
    int64_t clients = e.clients.count();
    double top_share = e.n.requests ? double(e.clients.top_requests()) / double(e.n.requests) : 0;
    o["heat_us"] = e.n.us;
    o["bytes"] = bj::object{{"in", e.n.bytes_in}, {"out", e.n.bytes_out}, {"max_in", e.n.max_in}};
    o["latency_us"] = e.n.latency_json();
    o["per_minute"] = bj::object{{"peak", int64_t(peak)}, {"peak_at_ms", started_ms + peak_at * 60000}};
    bj::object c{{"distinct", clients},
                 {"exact", e.clients.exact()},
                 {"busiest_minute", bj::object{{"p50", int64_t(busiest.percentile(0.5))},
                                               {"p99", int64_t(busiest.percentile(0.99))},
                                               {"max", int64_t(busiest.max())}}},
                 {"top_share", round4(top_share)}};
    if (e.clients.overflow) c["unattributed_requests"] = e.clients.overflow;
    o["clients"] = std::move(c);
    o["concurrent"] = bj::object{{"peak", int64_t(e.overlap.peak)}, {"per_client_peak", int64_t(e.overlap.peak_same)}};
    o["status"] = e.n.status_json();
    if (e.tracked_from_ms > started_ms) {
      o["tracked_from_ms"] = e.tracked_from_ms;
      o["before_tracked"] = e.prior;
    }
    SuggestInput in;
    in.study_id = id;
    in.domain = domain;
    in.api = e.api;
    in.endpoint = e.endpoint;
    in.requests = e.n.requests;
    in.clients = clients;
    in.p99_client_minute = busiest.percentile(0.99);
    in.peak_minute = peak;
    in.peak_concurrent = e.overlap.peak;
    in.peak_client_concurrent = e.overlap.peak_same;
    in.heat_share = heat_share;
    in.top_client_share = top_share;
    in.max_body_bytes = e.n.max_in;
    in.max_us = e.n.max_us;
    o["suggest"] = suggest_controls(in);
    return o;
  }

  // The domain's clients, from the endpoints' tables: distinct (a union of their estimates), and
  // the ten that asked most, by keyed hash, with the endpoint each asked most.
  bj::object clients_json(const std::vector<const Endpoint*>& eps) const {
    DistinctCounter distinct;
    struct Seen {
      uint64_t log_h = 0;
      int64_t requests = 0, most = 0;
      const Endpoint* where = nullptr;
    };
    std::unordered_map<uint64_t, Seen> seen;
    for (const Endpoint* e : eps) {
      distinct.merge(e->clients.distinct);
      for (const auto& [h, c] : e->clients.live) {
        Seen& s = seen[h];
        s.log_h = c.log_h;
        s.requests += c.requests;
        if (int64_t(c.requests) > s.most) s.most = c.requests, s.where = e;
      }
    }
    std::vector<const Seen*> top;
    for (const auto& [h, s] : seen) top.push_back(&s);
    std::sort(top.begin(), top.end(), [](const Seen* x, const Seen* y) {
      return x->requests != y->requests ? x->requests > y->requests : x->log_h < y->log_h;
    });
    bj::array rows;
    for (size_t i = 0; i < top.size() && i < 10; ++i) {
      rows.push_back(bj::object{{"client_h", hex16(top[i]->log_h)},
                                {"requests", top[i]->requests},
                                {"endpoint", top[i]->where->endpoint},
                                {"api", top[i]->where->api}});
    }
    return bj::object{{"distinct", distinct.estimate()}, {"top", std::move(rows)}};
  }

  bj::object report(int64_t now_ms, const std::string& state, const std::string& ended_by, int64_t ended_ms,
                    size_t detailed) const {
    int64_t end = state == "running" ? std::min(now_ms, ends_ms) : ended_ms;
    int64_t elapsed = std::max<int64_t>(0, end - started_ms);
    int64_t elapsed_minutes = std::max<int64_t>(1, (elapsed + 59999) / 60000);
    bj::object out = summary(state, ended_by, ended_ms);
    out["elapsed_ms"] = elapsed;
    out["saved_ms"] = saved_ms;
    out["totals"] = bj::object{{"requests", all.requests},       {"heat_us", all.us},
                               {"bytes_in", all.bytes_in},       {"bytes_out", all.bytes_out},
                               {"status", all.status_json()},    {"latency_us", all.latency_json()}};
    bj::array series;
    for (int64_t i = 0; i < elapsed_minutes; ++i) series.push_back(i < int64_t(all.minutes.size()) ? int64_t(all.minutes[size_t(i)]) : 0);
    auto [peak, peak_at] = all.peak_minute();
    out["per_minute"] = bj::object{{"series", std::move(series)}, {"peak", int64_t(peak)}, {"peak_at_ms", started_ms + peak_at * 60000}};
    std::vector<const Endpoint*> eps;
    for (const auto& [k, e] : endpoints) eps.push_back(e.get());
    if (other && other->n.requests > 0) eps.push_back(other.get());
    out["clients"] = clients_json(eps);
    // Ranked by heat; ties by requests, then by name, so a report reads the same each time.
    std::sort(eps.begin(), eps.end(), [](const Endpoint* x, const Endpoint* y) {
      if (x->n.us != y->n.us) return x->n.us > y->n.us;
      if (x->n.requests != y->n.requests) return x->n.requests > y->n.requests;
      return x->endpoint < y->endpoint;
    });
    bj::array rows;
    for (size_t i = 0; i < eps.size(); ++i) rows.push_back(endpoint_json(*eps[i], i < detailed));
    out["endpoints"] = std::move(rows);
    out["tracked"] = bj::object{{"endpoints", int64_t(endpoints.size())},
                                {"cap", int64_t(caps.endpoints)},
                                {"other_requests", other ? other->n.requests : 0},
                                {"promoted", promoted}};
    out["memory_bytes"] = memory();
    out["notes"] = bj::array{
        "heat is the time spent serving (µs); a client is the host's keyed hash of its rate-limit key: an IPv4 "
        "address (the audit log's client_h for the same address), or an IPv6 /64, whose hash no log line carries; "
        "no address and no user id is kept",
        "a client's busiest minute is its most requests on the endpoint in one minute of the study; concurrency is "
        "counted from finished requests, exact up to " + std::to_string(caps.endpoint_ring) + " at once",
        "suggestions are in observe mode: send one with control_set, read control_report, and enforce once it holds"};
    return out;
  }
};

// ------------------------------------------------------------------ the set

HeatStudies::HeatStudies(fs::path dir) : HeatStudies(std::move(dir), Caps{}) {}

HeatStudies::HeatStudies(fs::path dir, Caps caps, ClientHash hash)
    : dir_(std::move(dir)), caps_(caps), hash_(hash ? std::move(hash) : ClientHash([](std::string_view k) { return mix(hash_of(k)); })) {
  std::error_code ec;
  fs::create_directories(dir_, ec);
  for (auto it = fs::directory_iterator(dir_, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const auto& p = it->path();
    if (p.extension() != ".json") continue;
    std::string stem = p.stem().string();
    if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos || stem.size() > 15) continue;
    int64_t id = std::stoll(stem);
    next_id_ = std::max(next_id_, id + 1);
    auto text = base::read_file(p);
    std::string err;
    auto v = text ? json::parse(*text, &err) : std::nullopt;
    if (!v || !v->is_object()) {
      recovered_.push_back("heat study " + stem + ": " + p.string() + " doesn't parse; left as it is");
      continue;
    }
    auto& o = v->as_object();
    if (const auto* st = o.if_contains("state"); st && st->is_string() && st->as_string() == "running") {
      // Cut short by a crash or a kill: what was saved (at most a minute old) is the study.
      o["state"] = "interrupted";
      o["ended_by"] = "restart";
      int64_t saved = o.contains("saved_ms") && o.at("saved_ms").is_int64() ? o.at("saved_ms").as_int64() : 0;
      o["ended_ms"] = saved;
      o.erase("remaining_ms");
      base::write_file_durable(p, json::dump(o));
      recovered_.push_back("heat study " + stem + " was running when the host stopped; kept as interrupted");
    }
    Row row;
    for (const char* k : {"id", "domain", "tenant", "state", "why", "by", "minutes", "started_ms", "ends_ms", "ended_ms",
                          "ended_by", "stopped_by", "requests", "endpoints", "hottest"}) {
      if (const auto* x = o.if_contains(k)) row.summary[k] = *x;
    }
    row.summary["id"] = id;
    row.bytes = int64_t(fs::file_size(p, ec));
    done_[id] = std::move(row);
  }
}

HeatStudies::~HeatStudies() = default;

Study* HeatStudies::find(std::string_view domain) const {
  if (running_.load(std::memory_order_relaxed) == 0) return nullptr;
  for (const auto& slot : slots_) {
    Study* st = slot.load(std::memory_order_acquire);
    if (st && st->domain == domain) return st;
  }
  return nullptr;
}

bool HeatStudies::watching(std::string_view domain) const { return find(domain) != nullptr; }

void HeatStudies::record(std::string_view domain, const HeatSample& sample) {
  Study* st = find(domain);
  if (!st) return;
  // A visitor may choose the name (a path, a tool): at most kEndpointNameMax bytes of it, cut
  // on a character's boundary, so a long one is neither kept nor told apart from its prefix.
  HeatSample s = sample;
  if (s.what.size() > kEndpointNameMax) {
    size_t n = kEndpointNameMax;
    while (n > 0 && (static_cast<unsigned char>(s.what[n]) & 0xC0) == 0x80) --n;
    s.what = s.what.substr(0, n);
  }
  std::lock_guard lock(st->mu);
  st->add(s);
}

bool HeatStudies::write(int64_t id, const bj::object& report, int64_t* bytes) {
  std::string body = json::dump(report);
  *bytes = int64_t(body.size());
  return base::write_file_durable(dir_ / (std::to_string(id) + ".json"), body);
}

std::optional<bj::object> HeatStudies::start(const std::string& domain, const std::string& tenant, int minutes,
                                             const std::string& why, const std::string& by, int64_t now_ms,
                                             std::string* error) {
  if (minutes < kMinMinutes || minutes > kMaxMinutes) {
    return *error = "minutes is " + std::to_string(kMinMinutes) + " to " + std::to_string(kMaxMinutes), std::nullopt;
  }
  auto st = std::make_shared<Study>();
  {
    std::lock_guard lock(mu_);
    for (const auto& [id, x] : live_) {
      if (x->domain == domain) {
        return *error = "heat study " + std::to_string(id) + " already runs on " + domain +
                        " (one per domain); heat_study_stop it or wait for it to end",
               std::nullopt;
      }
    }
    auto free = std::find_if(slots_.begin(), slots_.end(), [](const auto& s) { return !s.load(std::memory_order_relaxed); });
    if (free == slots_.end()) {
      return *error = std::to_string(kMaxRunning) + " heat studies already run on this host (the most at once); stop one or wait",
             std::nullopt;
    }
    st->caps = caps_;
    st->hash = &hash_;
    st->id = next_id_++;
    st->domain = domain;
    st->tenant = tenant;
    st->why = why;
    st->by = by;
    st->minutes = minutes;
    st->started_ms = now_ms;
    st->ends_ms = now_ms + int64_t(minutes) * 60000;
    st->saved_ms = now_ms;
    st->other = std::make_unique<Endpoint>("other", "other", caps_);
    live_[st->id] = st;
    free->store(st.get(), std::memory_order_release);
    running_.fetch_add(1, std::memory_order_relaxed);
  }
  // Written at once, so a crash in the first minute still leaves a record.
  bj::object rep, row;
  {
    std::lock_guard lock(st->mu);
    rep = st->report(now_ms, "running", "", 0, caps_.detailed);
    row = st->summary("running", "", 0);
  }
  {
    std::lock_guard w(st->write_mu);
    bool ended = false;
    {
      std::lock_guard lock(st->mu);
      ended = st->ended;  // stopped already: its final report is written, or will be after this
    }
    if (!ended) {
      int64_t bytes = 0;
      write(st->id, rep, &bytes);
      st->file_bytes = bytes;
    }
  }
  return row;
}

std::optional<bj::object> HeatStudies::finish(const std::shared_ptr<Study>& st, const std::string& state,
                                              const std::string& ended_by, int64_t ended_ms) {
  bj::object rep, row;
  {
    std::lock_guard lock(st->mu);
    if (st->ended) return std::nullopt;
    st->ended = true;
    st->saved_ms = ended_ms;
    // Small: past kReportMaxBytes, the detail of the cooler endpoints goes first.
    for (size_t detailed : {caps_.detailed, size_t(50), size_t(20), size_t(0)}) {
      rep = st->report(ended_ms, state, ended_by, ended_ms, std::min(detailed, caps_.detailed));
      if (json::dump(rep).size() <= kReportMaxBytes) break;
    }
    row = st->summary(state, ended_by, ended_ms);
    st->release();
    st->final_report = rep;
  }
  int64_t bytes = 0;
  {
    std::lock_guard w(st->write_mu);
    write(st->id, rep, &bytes);
  }
  {
    std::lock_guard lock(st->mu);
    st->final_report = {};
  }
  std::lock_guard lock(mu_);
  for (auto& slot : slots_) {
    if (slot.load(std::memory_order_relaxed) == st.get()) slot.store(nullptr, std::memory_order_release);
  }
  if (live_.erase(st->id)) running_.fetch_sub(1, std::memory_order_relaxed);
  ended_.push_back(st);
  done_[st->id] = Row{row, bytes};
  return row;
}

std::optional<bj::object> HeatStudies::stop(int64_t id, const std::string& by, int64_t now_ms, std::string* error) {
  std::shared_ptr<Study> st;
  {
    std::lock_guard lock(mu_);
    if (auto it = live_.find(id); it != live_.end()) {
      st = it->second;
    } else if (auto d = done_.find(id); d != done_.end()) {
      const auto* s = d->second.summary.if_contains("state");
      return *error = "heat study " + std::to_string(id) + " already ended (" +
                      (s && s->is_string() ? std::string(s->as_string()) : std::string("done")) + "); heat_study_get reads it",
             std::nullopt;
    } else {
      return *error = "no heat study " + std::to_string(id), std::nullopt;
    }
  }
  {
    std::lock_guard lock(st->mu);
    if (!st->ended) st->stopped_by = by;
  }
  auto row = finish(st, "stopped", "operator", now_ms);
  if (!row) return *error = "heat study " + std::to_string(id) + " already ended", std::nullopt;
  return row;
}

std::optional<bj::object> HeatStudies::get(int64_t id, int64_t now_ms, std::string* error) const {
  std::shared_ptr<Study> st;
  {
    std::lock_guard lock(mu_);
    if (auto it = live_.find(id); it != live_.end()) {
      st = it->second;
    } else if (!done_.count(id)) {
      return *error = "no heat study " + std::to_string(id) + " (finished ones are kept 30 days)", std::nullopt;
    }
  }
  if (st) {
    std::lock_guard lock(st->mu);
    if (!st->ended) {
      auto rep = st->report(now_ms, "running", "", 0, caps_.detailed);
      rep["remaining_ms"] = std::max<int64_t>(0, st->ends_ms - now_ms);
      return rep;
    }
    if (!st->final_report.empty()) return st->final_report;
  }
  auto text = base::read_file(dir_ / (std::to_string(id) + ".json"));
  std::string err;
  auto v = text ? json::parse(*text, &err) : std::nullopt;
  if (!v || !v->is_object()) return *error = "heat study " + std::to_string(id) + "'s report can't be read", std::nullopt;
  return v->as_object();
}

bj::array HeatStudies::list(const std::string& domain, int64_t now_ms) const {
  std::vector<std::pair<int64_t, bj::object>> rows;
  std::vector<std::shared_ptr<Study>> live;
  {
    std::lock_guard lock(mu_);
    for (const auto& [id, st] : live_) live.push_back(st);
    for (const auto& [id, r] : done_) rows.emplace_back(id, r.summary);
  }
  for (const auto& st : live) {
    std::lock_guard lock(st->mu);
    if (st->ended) continue;
    auto row = st->summary("running", "", 0);
    row["remaining_ms"] = std::max<int64_t>(0, st->ends_ms - now_ms);
    rows.emplace_back(st->id, std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  bj::array out;
  for (auto& [id, row] : rows) {
    if (!domain.empty()) {
      const auto* d = row.if_contains("domain");
      if (!d || !d->is_string() || d->as_string() != domain) continue;
    }
    out.push_back(std::move(row));
  }
  return out;
}

std::optional<std::string> HeatStudies::tenant_of(int64_t id) const {
  std::lock_guard lock(mu_);
  if (auto it = live_.find(id); it != live_.end()) return it->second->tenant;
  if (auto d = done_.find(id); d != done_.end()) {
    if (const auto* t = d->second.summary.if_contains("tenant"); t && t->is_string()) return std::string(t->as_string());
  }
  return std::nullopt;
}

std::vector<bj::object> HeatStudies::tick(int64_t now_ms, int64_t budget_bytes) {
  std::vector<bj::object> ended;
  if (running_.load(std::memory_order_relaxed) == 0) return ended;
  std::vector<std::shared_ptr<Study>> live;
  {
    std::lock_guard lock(mu_);
    for (const auto& [id, st] : live_) live.push_back(st);
  }
  for (const auto& st : live) {
    bool due = false;
    bj::object rep;
    {
      std::lock_guard lock(st->mu);
      if (st->ended) continue;
      due = now_ms >= st->ends_ms;
      if (!due) {
        st->promote(now_ms);
        if (now_ms - st->saved_ms >= kSaveEveryMs) {
          st->saved_ms = now_ms;
          rep = st->report(now_ms, "running", "", 0, caps_.detailed);
        }
      }
    }
    if (due) {
      if (auto row = finish(st, "done", "time", st->ends_ms)) ended.push_back(std::move(*row));
    } else if (!rep.empty()) {
      if (before_save_) before_save_(st->id);
      std::lock_guard w(st->write_mu);
      bool ended = false;
      {
        std::lock_guard lock(st->mu);
        ended = st->ended;  // a stop came between: its final report stands
      }
      if (!ended) {
        int64_t bytes = 0;
        write(st->id, rep, &bytes);
        st->file_bytes = bytes;
      }
    }
  }
  if (!ended.empty() && budget_bytes > 0) prune(now_ms, budget_bytes);
  return ended;
}

std::vector<bj::object> HeatStudies::stop_all(int64_t now_ms) {
  std::vector<bj::object> ended;
  std::vector<std::shared_ptr<Study>> live;
  {
    std::lock_guard lock(mu_);
    for (const auto& [id, st] : live_) live.push_back(st);
  }
  for (const auto& st : live) {
    if (auto row = finish(st, "stopped", "host_stopping", std::min(now_ms, st->ends_ms))) ended.push_back(std::move(*row));
  }
  return ended;
}

int HeatStudies::prune(int64_t now_ms, int64_t budget_bytes) {
  std::lock_guard lock(mu_);
  int gone = 0;
  auto ended_of = [](const Row& r) {
    const auto* e = r.summary.if_contains("ended_ms");
    return e && e->is_int64() ? e->as_int64() : int64_t(0);
  };
  auto drop = [&](std::map<int64_t, Row>::iterator it) {
    std::error_code ec;
    fs::remove(dir_ / (std::to_string(it->first) + ".json"), ec);
    ++gone;
    return done_.erase(it);
  };
  for (auto it = done_.begin(); it != done_.end();) {
    it = ended_of(it->second) + kKeepMs <= now_ms ? drop(it) : std::next(it);
  }
  if (budget_bytes > 0) {
    int64_t total = 0;
    for (const auto& [id, r] : done_) total += r.bytes;
    for (const auto& [id, st] : live_) total += st->file_bytes;
    while (total > budget_bytes && !done_.empty()) {
      auto oldest = done_.begin();
      for (auto it = done_.begin(); it != done_.end(); ++it) {
        if (ended_of(it->second) < ended_of(oldest->second)) oldest = it;
      }
      total -= oldest->second.bytes;
      drop(oldest);
    }
  }
  return gone;
}

int64_t HeatStudies::bytes() const {
  std::lock_guard lock(mu_);
  int64_t total = 0;
  for (const auto& [id, r] : done_) total += r.bytes;
  for (const auto& [id, st] : live_) total += st->file_bytes;
  return total;
}

std::vector<std::string> HeatStudies::recovered() const {
  std::lock_guard lock(mu_);
  return recovered_;
}

}  // namespace goober::operations
