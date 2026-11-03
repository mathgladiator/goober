#include "goober/operations/heat.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <system_error>

#include "goober/base/files.h"
#include "goober/json/json.h"

namespace goober::operations {
namespace bj = boost::json;
namespace fs = std::filesystem;

// ------------------------------------------------------------------ small pieces

ClientClass classify_client(std::string_view user_agent, bool agent) {
  if (agent) return ClientClass::kAgent;
  std::string ua;
  ua.reserve(std::min<size_t>(user_agent.size(), 512));
  for (char c : user_agent.substr(0, 512)) ua.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  static const char* const kBots[] = {"bot",     "crawl",  "spider",         "slurp",       "curl/",       "wget/",
                                      "python",  "go-http-client", "java/", "libwww",      "scrapy",      "httpclient",
                                      "okhttp",  "axios",  "node-fetch",     "headless",    "monitor",     "uptime",
                                      "feedfetcher", "facebookexternalhit", "lighthouse"};
  for (const char* b : kBots) {
    if (ua.find(b) != std::string::npos) return ClientClass::kBot;
  }
  if (ua.starts_with("mozilla/")) return ClientClass::kBrowser;
  return ClientClass::kOther;
}

const char* client_class_name(ClientClass c) {
  switch (c) {
    case ClientClass::kBrowser:
      return "browser";
    case ClientClass::kBot:
      return "bot";
    case ClientClass::kAgent:
      return "agent";
    case ClientClass::kOther:
      break;
  }
  return "other";
}

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
    std::string name;
    for (char c : inside) {
      if (!std::isspace(static_cast<unsigned char>(c))) name.push_back(c);
    }
    out += "{" + name + "}";
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
std::string clients_text(int64_t n) { return std::to_string(n) + (n == 1 ? " client" : " clients"); }

}  // namespace

bj::object suggest_controls(const SuggestInput& in) {
  if (in.api == "other" || in.api == "host" || in.endpoint.empty() || in.endpoint.front() == '(') {
    return bj::object{{"none", "not an endpoint a control names"}};
  }
  if (in.requests < 20) {
    return bj::object{{"none", "only " + std::to_string(in.requests) +
                                   " requests: too few to suggest from; study longer, or at a busier time"}};
  }
  bj::object args{{"domain", in.domain}, {"api", in.api}, {"match", in.endpoint}};
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
                      "% of the domain's heat over " + clients_text(in.clients) +
                      ", none above 10% of its requests: per-minute " + std::to_string(per_minute) +
                      " = max(10, 2 x " + std::to_string(in.peak_minute) +
                      ", the endpoint's busiest minute); burst is half that";
  } else {
    by = "client";
    per_minute = std::max<int64_t>(10, 2 * int64_t(in.p99_client_minute));
    reasons["rate"] = "per-minute " + std::to_string(per_minute) + " = max(10, 2 x " +
                      std::to_string(in.p99_client_minute) + ", the p99 of each client's busiest minute over " +
                      clients_text(in.clients) + "); burst is half that";
  }
  int64_t burst = std::max<int64_t>(1, (per_minute + 1) / 2);
  args["rate"] = bj::object{{"per-minute", per_minute}, {"burst", burst}, {"by", by}};
  int64_t cmax = std::max<int64_t>(2, 2 * in.peak_concurrent);
  int64_t cper = std::max<int64_t>(1, 2 * in.peak_client_concurrent);
  args["concurrent"] = bj::object{{"max", cmax}, {"per-client", cper}};
  reasons["concurrent"] = "max " + std::to_string(cmax) + " = max(2, 2 x " + std::to_string(in.peak_concurrent) +
                          ", the most in flight at once); per-client " + std::to_string(cper) + " = max(1, 2 x " +
                          std::to_string(in.peak_client_concurrent) + ", the most one client had in flight)";
  if (in.max_body_bytes > 0) {
    int64_t b = std::max<int64_t>(1024, round_up(2 * in.max_body_bytes, 1024));
    args["body-max-bytes"] = b;
    reasons["body-max-bytes"] = std::to_string(b) + " = max(1024, 2 x " + std::to_string(in.max_body_bytes) +
                                " bytes, the largest body seen, rounded up to a KiB)";
  }
  if (in.api.starts_with("box:")) {
    int64_t ms = std::max<int64_t>(1000, round_up((2 * in.max_us + 999) / 1000, 100));
    args["timeout-ms"] = ms;
    reasons["timeout-ms"] = std::to_string(ms) + " = max(1000, 2 x " + std::to_string((in.max_us + 999) / 1000) +
                            " ms, the slowest answer, rounded up to 100 ms)";
  }
  args["mode"] = "observe";
  args["why"] = "heat study " + std::to_string(in.study_id) + ": observe first, then enforce";
  return bj::object{{"control_set", std::move(args)}, {"reasons", std::move(reasons)}};
}

// ------------------------------------------------------------------ accumulators

namespace {

struct ClientState {
  int64_t minute = -1;
  uint32_t in_minute = 0, best = 0, requests = 0;
  ClientClass cls = ClientClass::kOther;
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

  void add(uint64_t client, int64_t minute, ClientClass cls) {
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
      it->second.cls = cls;
    }
    ClientState& c = it->second;
    ++c.requests;
    if (c.minute == minute || minute < c.minute) {
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
// `cap` finished on the same key, both ways, so the count at every start is exact while no more
// than `cap` overlap.
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
  std::string api, endpoint, method;
  int64_t tracked_from_ms = 0;
  int64_t prior = 0;  // requests counted in "other" before it was tracked
  int64_t requests = 0, us = 0, fuel = 0, bytes_in = 0, bytes_out = 0, max_in = 0, max_us = 0;
  std::array<int64_t, 5> status{};   // 2xx, 3xx, 4xx (not 429), 5xx, 429
  std::array<int64_t, 4> classes{};  // browser, bot, agent, other
  std::vector<uint32_t> minutes;     // requests per minute since the study began
  Histogram latency;
  ClientTable clients;
  Overlap overlap;

  Endpoint(std::string a, std::string e, std::string m, size_t client_cap, size_t ring_cap)
      : api(std::move(a)), endpoint(std::move(e)), method(std::move(m)) {
    clients.cap = client_cap;
    overlap.cap = ring_cap;
  }

  void add(const HeatSample& s, int64_t minute) {
    ++requests;
    us += std::max<int64_t>(0, s.us);
    fuel += std::max<int64_t>(0, s.fuel);
    bytes_in += s.bytes_in;
    bytes_out += s.bytes_out;
    max_in = std::max(max_in, s.bytes_in);
    max_us = std::max(max_us, s.us);
    int st = s.status;
    status[st == 429 ? 4 : st >= 500 ? 3 : st >= 400 ? 2 : st >= 300 ? 1 : 0] += 1;
    classes[size_t(s.cls)] += 1;
    if (minutes.size() <= size_t(minute)) minutes.resize(size_t(minute) + 1);
    ++minutes[size_t(minute)];
    latency.add(std::max<int64_t>(0, s.us));
    clients.add(s.client, minute, s.cls);
    overlap.add(s.end_us - std::max<int64_t>(0, s.us), s.end_us, s.client);
  }

  void fold_into(Endpoint& o) const {
    o.requests += requests;
    o.us += us;
    o.fuel += fuel;
    o.bytes_in += bytes_in;
    o.bytes_out += bytes_out;
    o.max_in = std::max(o.max_in, max_in);
    o.max_us = std::max(o.max_us, max_us);
    for (size_t i = 0; i < status.size(); ++i) o.status[i] += status[i];
    for (size_t i = 0; i < classes.size(); ++i) o.classes[i] += classes[i];
    if (o.minutes.size() < minutes.size()) o.minutes.resize(minutes.size());
    for (size_t i = 0; i < minutes.size(); ++i) o.minutes[i] += minutes[i];
    o.latency.merge(latency);
    o.clients.done.merge(clients.busiest());
    o.clients.done_top = std::max(o.clients.done_top, clients.top_requests());
    o.clients.distinct.merge(clients.distinct);
    o.clients.overflow += clients.overflow;
    o.clients.evicted = true;
    o.overlap.peak = std::max(o.overlap.peak, overlap.peak);
    o.overlap.peak_same = std::max(o.overlap.peak_same, overlap.peak_same);
  }

  int64_t memory() const {
    return int64_t(sizeof(Endpoint)) + int64_t(minutes.capacity()) * 4 + int64_t(clients.live.size()) * 56 +
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
  int64_t id = 0;
  std::string domain, tenant, why, by, stopped_by;
  int minutes = 60;
  int64_t started_ms = 0, ends_ms = 0, saved_ms = 0;
  std::atomic<int64_t> file_bytes{0};
  int64_t promoted_minute = -1;
  bool ended = false;
  std::map<std::string, std::unique_ptr<Endpoint>> endpoints;  // api \n endpoint
  struct Candidate {
    std::string api, endpoint, method;
    int64_t requests = 0;
  };
  std::unordered_map<std::string, Candidate> candidates;
  int64_t candidates_dropped = 0, promoted = 0;
  std::unique_ptr<Endpoint> other, all;

  void add(const HeatSample& s) {
    if (ended || s.at_ms < started_ms || s.at_ms >= ends_ms) return;
    int64_t minute = (s.at_ms - started_ms) / 60000;
    all->add(s, minute);
    std::string key = s.api + "\n" + s.endpoint;
    auto it = endpoints.find(key);
    if (it == endpoints.end()) {
      if (endpoints.size() < caps.endpoints) {
        auto ep = std::make_unique<Endpoint>(s.api, s.endpoint, s.method, caps.endpoint_clients, caps.endpoint_ring);
        ep->tracked_from_ms = started_ms;
        it = endpoints.emplace(key, std::move(ep)).first;
      } else {
        if (auto c = candidates.find(key); c != candidates.end()) {
          ++c->second.requests;
        } else if (candidates.size() < caps.candidates) {
          candidates.emplace(key, Candidate{s.api, s.endpoint, s.method, 1});
        } else {
          ++candidates_dropped;
        }
        other->add(s, minute);
        return;
      }
    }
    it->second->add(s, minute);
  }

  // Once a minute: an untracked endpoint busier than the least busy tracked one takes its place;
  // the one it replaces is folded into "other".
  void promote(int64_t now_ms) {
    int64_t minute = (now_ms - started_ms) / 60000;
    if (minute == promoted_minute) return;
    promoted_minute = minute;
    if (candidates.empty() || endpoints.empty()) return;
    std::vector<std::pair<int64_t, std::string>> ranked;
    for (const auto& [k, c] : candidates) ranked.emplace_back(c.requests, k);
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < ranked.size() && i < 50; ++i) {
      auto low = endpoints.begin();
      for (auto e = endpoints.begin(); e != endpoints.end(); ++e) {
        if (e->second->requests + e->second->prior < low->second->requests + low->second->prior) low = e;
      }
      int64_t low_n = low->second->requests + low->second->prior;
      if (ranked[i].first <= low_n) break;
      low->second->fold_into(*other);
      Candidate back{low->second->api, low->second->endpoint, low->second->method, low_n};
      std::string back_key = low->first;
      endpoints.erase(low);
      auto c = candidates.find(ranked[i].second);
      auto ep = std::make_unique<Endpoint>(c->second.api, c->second.endpoint, c->second.method, caps.endpoint_clients,
                                           caps.endpoint_ring);
      ep->tracked_from_ms = now_ms;
      ep->prior = c->second.requests;
      endpoints.emplace(c->first, std::move(ep));
      candidates.erase(c);
      candidates[back_key] = std::move(back);
      ++promoted;
    }
  }

  int64_t memory() const {
    int64_t m = int64_t(sizeof(Study)) + other->memory() + all->memory();
    for (const auto& [k, e] : endpoints) m += e->memory() + int64_t(k.size()) + 48;
    m += int64_t(candidates.size()) * 96;
    return m;
  }

  bj::object summary(const std::string& state, const std::string& ended_by, int64_t ended_ms) const {
    bj::object row{{"id", id},          {"domain", domain},        {"tenant", tenant}, {"state", state},
                   {"why", why},        {"by", by},                {"minutes", minutes}, {"started_ms", started_ms},
                   {"ends_ms", ends_ms}, {"requests", all->requests}, {"endpoints", int64_t(endpoints.size())}};
    if (state != "running") {
      row["ended_ms"] = ended_ms;
      row["ended_by"] = ended_by;
    }
    if (!stopped_by.empty()) row["stopped_by"] = stopped_by;
    const Endpoint* hottest = nullptr;
    for (const auto& [k, e] : endpoints) {
      if (!hottest || e->us > hottest->us) hottest = e.get();
    }
    if (hottest && all->us > 0) {
      row["hottest"] = bj::object{{"endpoint", hottest->endpoint}, {"api", hottest->api},
                                  {"heat_share", round4(double(hottest->us) / double(all->us))}};
    }
    return row;
  }

  bj::object endpoint_json(const Endpoint& e, bool detailed, bool series, int64_t elapsed_minutes) const {
    double total_us = double(std::max<int64_t>(1, all->us));
    double total_n = double(std::max<int64_t>(1, all->requests));
    double total_b = double(std::max<int64_t>(1, all->bytes_in + all->bytes_out));
    double heat_share = round4(double(e.us) / total_us);
    bj::object o{{"endpoint", e.endpoint},
                 {"api", e.api},
                 {"requests", e.requests},
                 {"heat_share", heat_share},
                 {"requests_share", round4(double(e.requests) / total_n)},
                 {"bytes_share", round4(double(e.bytes_in + e.bytes_out) / total_b)}};
    if (!detailed) return o;
    uint32_t peak = 0;
    int64_t peak_at = 0;
    for (size_t i = 0; i < e.minutes.size(); ++i) {
      if (e.minutes[i] > peak) peak = e.minutes[i], peak_at = int64_t(i);
    }
    auto busiest = e.clients.busiest();
    int64_t clients = e.clients.count();
    o["heat"] = bj::object{{"us", e.us}, {"fuel", e.fuel}};
    o["bytes"] = bj::object{{"in", e.bytes_in}, {"out", e.bytes_out}, {"max_in", e.max_in}};
    o["latency_us"] = bj::object{{"p50", std::min(e.latency.percentile(0.5), e.max_us)},
                                 {"p99", std::min(e.latency.percentile(0.99), e.max_us)},
                                 {"max", e.max_us}};
    o["per_minute"] = bj::object{{"peak", int64_t(peak)},
                                 {"peak_at_ms", started_ms + peak_at * 60000},
                                 {"mean", round4(double(e.requests) / double(std::max<int64_t>(1, elapsed_minutes)))}};
    double top_share = e.requests ? double(e.clients.top_requests()) / double(e.requests) : 0;
    o["clients"] = bj::object{{"distinct", clients},
                              {"exact", e.clients.exact()},
                              {"busiest_minute", bj::object{{"p50", int64_t(busiest.percentile(0.5))},
                                                            {"p99", int64_t(busiest.percentile(0.99))},
                                                            {"max", int64_t(busiest.max())}}},
                              {"top_share", round4(top_share)}};
    if (e.clients.overflow) o["clients"].as_object()["unattributed_requests"] = e.clients.overflow;
    o["concurrent"] = bj::object{{"peak", int64_t(e.overlap.peak)},
                                 {"per_client_peak", int64_t(e.overlap.peak_same)},
                                 {"exact_up_to", int64_t(e.overlap.cap)}};
    o["status"] = bj::object{{"2xx", e.status[0]}, {"3xx", e.status[1]}, {"4xx", e.status[2]},
                             {"5xx", e.status[3]}, {"429", e.status[4]}};
    o["classes"] = bj::object{{"browser", e.classes[0]}, {"bot", e.classes[1]}, {"agent", e.classes[2]},
                              {"other", e.classes[3]}};
    if (e.tracked_from_ms > started_ms) {
      o["tracked_from_ms"] = e.tracked_from_ms;
      o["before_tracked"] = e.prior;
    }
    if (series) {
      bj::array pts;
      for (int64_t i = 0; i < elapsed_minutes; ++i) pts.push_back(i < int64_t(e.minutes.size()) ? int64_t(e.minutes[size_t(i)]) : 0);
      o["series"] = std::move(pts);
    }
    SuggestInput in;
    in.study_id = id;
    in.domain = domain;
    in.api = e.api;
    in.endpoint = e.endpoint;
    in.method = e.method;
    in.requests = e.requests;
    in.clients = clients;
    in.p99_client_minute = busiest.percentile(0.99);
    in.peak_minute = peak;
    in.peak_concurrent = e.overlap.peak;
    in.peak_client_concurrent = e.overlap.peak_same;
    in.heat_share = heat_share;
    in.top_client_share = top_share;
    in.max_body_bytes = e.max_in;
    in.max_us = e.max_us;
    o["suggest"] = suggest_controls(in);
    return o;
  }

  bj::object report(int64_t now_ms, const std::string& state, const std::string& ended_by, int64_t ended_ms,
                    size_t detailed, size_t series) const {
    int64_t end = state == "running" ? std::min(now_ms, ends_ms) : ended_ms;
    int64_t elapsed = std::max<int64_t>(0, end - started_ms);
    int64_t elapsed_minutes = std::max<int64_t>(1, (elapsed + 59999) / 60000);
    bj::object out = summary(state, ended_by, ended_ms);
    out["elapsed_ms"] = elapsed;
    out["saved_ms"] = saved_ms;
    const Endpoint& a = *all;
    out["totals"] = bj::object{
        {"requests", a.requests},
        {"heat_us", a.us},
        {"fuel", a.fuel},
        {"bytes_in", a.bytes_in},
        {"bytes_out", a.bytes_out},
        {"status", bj::object{{"2xx", a.status[0]}, {"3xx", a.status[1]}, {"4xx", a.status[2]}, {"5xx", a.status[3]},
                              {"429", a.status[4]}}},
        {"classes", bj::object{{"browser", a.classes[0]}, {"bot", a.classes[1]}, {"agent", a.classes[2]},
                               {"other", a.classes[3]}}},
        {"latency_us", bj::object{{"p50", std::min(a.latency.percentile(0.5), a.max_us)},
                                  {"p99", std::min(a.latency.percentile(0.99), a.max_us)},
                                  {"max", a.max_us}}}};
    {
      bj::array pts;
      uint32_t peak = 0;
      int64_t peak_at = 0;
      for (int64_t i = 0; i < elapsed_minutes; ++i) {
        uint32_t v = i < int64_t(a.minutes.size()) ? a.minutes[size_t(i)] : 0;
        if (v > peak) peak = v, peak_at = i;
        pts.push_back(int64_t(v));
      }
      out["per_minute"] = bj::object{{"series", std::move(pts)},
                                     {"peak", int64_t(peak)},
                                     {"peak_at_ms", started_ms + peak_at * 60000},
                                     {"mean", round4(double(a.requests) / double(elapsed_minutes))}};
    }
    auto busiest = a.clients.busiest();
    out["clients"] = bj::object{
        {"distinct", a.clients.count()},
        {"exact", a.clients.exact()},
        {"busiest_minute", bj::object{{"p50", int64_t(busiest.percentile(0.5))}, {"p99", int64_t(busiest.percentile(0.99))}, {"max", int64_t(busiest.max())}}}};
    if (a.clients.overflow) out["clients"].as_object()["unattributed_requests"] = a.clients.overflow;
    out["concurrent"] = bj::object{{"peak", int64_t(a.overlap.peak)},
                                   {"per_client_peak", int64_t(a.overlap.peak_same)},
                                   {"exact_up_to", int64_t(a.overlap.cap)}};
    // The busiest clients, by keyed hash only: the same hash as the access log's client_h, so
    // the log dive finds their lines; never an address.
    {
      std::vector<std::pair<uint64_t, const ClientState*>> cs;
      for (const auto& [h, c] : a.clients.live) cs.emplace_back(h, &c);
      std::sort(cs.begin(), cs.end(), [](const auto& x, const auto& y) {
        return x.second->requests != y.second->requests ? x.second->requests > y.second->requests : x.first < y.first;
      });
      bj::array top;
      for (size_t i = 0; i < cs.size() && i < 10; ++i) {
        top.push_back(bj::object{{"client_h", hex16(cs[i].first)},
                                 {"requests", int64_t(cs[i].second->requests)},
                                 {"busiest_minute", int64_t(cs[i].second->busiest())},
                                 {"class", client_class_name(cs[i].second->cls)}});
      }
      out["top_clients"] = std::move(top);
    }
    std::vector<const Endpoint*> eps;
    for (const auto& [k, e] : endpoints) eps.push_back(e.get());
    if (other->requests > 0) eps.push_back(other.get());
    auto sorted_by = [&](auto key) {
      std::vector<const Endpoint*> v = eps;
      std::stable_sort(v.begin(), v.end(), [&](const Endpoint* x, const Endpoint* y) { return key(*x) > key(*y); });
      return v;
    };
    auto by_heat = sorted_by([](const Endpoint& e) { return std::make_pair(e.us, e.requests); });
    bj::array rows;
    for (size_t i = 0; i < by_heat.size(); ++i) {
      rows.push_back(endpoint_json(*by_heat[i], i < detailed, i < series, elapsed_minutes));
    }
    out["endpoints"] = std::move(rows);
    auto rank = [&](const std::vector<const Endpoint*>& v, auto share) {
      bj::array r;
      for (size_t i = 0; i < v.size() && i < 20; ++i) {
        r.push_back(bj::object{{"endpoint", v[i]->endpoint}, {"api", v[i]->api}, {"share", round4(share(*v[i]))}});
      }
      return r;
    };
    double tu = double(std::max<int64_t>(1, a.us)), tn = double(std::max<int64_t>(1, a.requests)),
           tb = double(std::max<int64_t>(1, a.bytes_in + a.bytes_out));
    out["ranks"] = bj::object{
        {"by_heat", rank(by_heat, [&](const Endpoint& e) { return double(e.us) / tu; })},
        {"by_requests", rank(sorted_by([](const Endpoint& e) { return e.requests; }), [&](const Endpoint& e) { return double(e.requests) / tn; })},
        {"by_bytes", rank(sorted_by([](const Endpoint& e) { return e.bytes_in + e.bytes_out; }),
                          [&](const Endpoint& e) { return double(e.bytes_in + e.bytes_out) / tb; })}};
    out["tracked"] = bj::object{{"endpoints", int64_t(endpoints.size())},
                                {"cap", int64_t(caps.endpoints)},
                                {"other_requests", other->requests},
                                {"candidates", int64_t(candidates.size())},
                                {"candidates_dropped", candidates_dropped},
                                {"promoted", promoted},
                                {"detailed", int64_t(std::min(detailed, by_heat.size()))}};
    out["memory_bytes"] = memory();
    out["notes"] = bj::array{
        "heat is the time spent serving each request (µs), the measure hot_boxes uses; fuel is the box's own count "
        "where a box ran",
        "a client is a keyed hash of its address (an IPv6 /64 is one), as the host's rate limiter counts clients; "
        "the report holds no address and no user id",
        "a client's busiest minute is its most requests in one minute of the study; distinct clients are exact while "
        "clients.exact is true, else a HyperLogLog estimate (about 6.5%)",
        "concurrency is exact up to exact_up_to requests in flight at once on an endpoint",
        "suggestions are in observe mode: send one with control_set, read control_report, and enforce once it holds"};
    return out;
  }
};

// ------------------------------------------------------------------ the set

HeatStudies::HeatStudies(fs::path dir) : HeatStudies(std::move(dir), Caps{}) {}

HeatStudies::HeatStudies(fs::path dir, Caps caps) : dir_(std::move(dir)), caps_(caps) {
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
      std::string body = json::dump(o);
      base::write_file_durable(p, body);
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

bool HeatStudies::watching(std::string_view domain) const {
  if (running_.load(std::memory_order_relaxed) == 0) return false;
  std::lock_guard lock(mu_);
  for (const auto& [id, st] : live_) {
    if (st->domain == domain) return true;
  }
  return false;
}

void HeatStudies::record(std::string_view domain, const HeatSample& s) {
  if (running_.load(std::memory_order_relaxed) == 0) return;
  std::shared_ptr<Study> st;
  {
    std::lock_guard lock(mu_);
    for (const auto& [id, x] : live_) {
      if (x->domain == domain) st = x;
    }
  }
  if (!st) return;
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
    if (int(live_.size()) >= kMaxRunning) {
      return *error = std::to_string(kMaxRunning) + " heat studies already run on this host (the most at once); stop one or wait",
             std::nullopt;
    }
    st->caps = caps_;
    st->id = next_id_++;
    st->domain = domain;
    st->tenant = tenant;
    st->why = why;
    st->by = by;
    st->minutes = minutes;
    st->started_ms = now_ms;
    st->ends_ms = now_ms + int64_t(minutes) * 60000;
    st->saved_ms = now_ms;
    st->other = std::make_unique<Endpoint>("other", "other", "", caps_.endpoint_clients, caps_.endpoint_ring);
    st->all = std::make_unique<Endpoint>("all", "all", "", caps_.domain_clients, caps_.domain_ring);
    live_[st->id] = st;
    running_.fetch_add(1, std::memory_order_relaxed);
  }
  // Written at once, so a crash in the first minute still leaves a record.
  bj::object rep;
  {
    std::lock_guard lock(st->mu);
    rep = st->report(now_ms, "running", "", 0, caps_.detailed, caps_.series);
  }
  int64_t bytes = 0;
  write(st->id, rep, &bytes);
  {
    std::lock_guard lock(st->mu);
    st->file_bytes = bytes;
    return st->summary("running", "", 0);
  }
}

std::optional<bj::object> HeatStudies::finish(const std::shared_ptr<Study>& st, const std::string& state,
                                              const std::string& ended_by, int64_t ended_ms) {
  bj::object rep, row;
  {
    std::lock_guard lock(st->mu);
    if (st->ended) return std::nullopt;
    st->ended = true;
    st->saved_ms = ended_ms;
    // Small: past kReportMaxBytes, the series go first, then the detail of the cooler endpoints.
    const std::pair<size_t, size_t> shapes[] = {{caps_.detailed, caps_.series}, {caps_.detailed, 0}, {50, 0}, {20, 0}, {0, 0}};
    for (auto [detailed, series] : shapes) {
      rep = st->report(ended_ms, state, ended_by, ended_ms, detailed, series);
      if (json::dump(rep).size() <= kReportMaxBytes) break;
    }
    row = st->summary(state, ended_by, ended_ms);
  }
  int64_t bytes = 0;
  write(st->id, rep, &bytes);
  std::lock_guard lock(mu_);
  if (live_.erase(st->id)) running_.fetch_sub(1, std::memory_order_relaxed);
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
    st->stopped_by = by;
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
    auto rep = st->report(now_ms, "running", "", 0, caps_.detailed, caps_.series);
    rep["remaining_ms"] = std::max<int64_t>(0, st->ends_ms - now_ms);
    return rep;
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
    bool due = false, save = false;
    {
      std::lock_guard lock(st->mu);
      if (st->ended) continue;
      due = now_ms >= st->ends_ms;
      if (!due) {
        st->promote(now_ms);
        save = now_ms - st->saved_ms >= kSaveEveryMs;
      }
    }
    if (due) {
      if (auto row = finish(st, "done", "time", st->ends_ms)) ended.push_back(std::move(*row));
      continue;
    }
    if (save) {
      bj::object rep;
      {
        std::lock_guard lock(st->mu);
        st->saved_ms = now_ms;
        rep = st->report(now_ms, "running", "", 0, caps_.detailed, caps_.series);
      }
      int64_t bytes = 0;
      write(st->id, rep, &bytes);
      std::lock_guard lock(st->mu);
      st->file_bytes = bytes;
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
