// Performance recording and reports (design/machine/037.Caching.md § 7).

#include "goober/host/perf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

constexpr int64_t kMinuteMs = 60'000;
constexpr int64_t kSlotMs = 10 * kMinuteMs;
constexpr int64_t kHourMs = 60 * kMinuteMs;

// Findings' thresholds: high enough that one slow call is not a pattern.
constexpr int64_t kCacheableCalls = 30;         // a GET read this often in the window...
constexpr int64_t kCacheableWritesPer = 20;     // ...whose databases changed at most once per this many reads
constexpr int64_t kScanStepsPerRun = 100;       // full-scan steps per run that say an index is missing
constexpr int64_t kScanRuns = 5;
constexpr int64_t kLockWaitUs = 5'000;          // average wait for the write lock...
constexpr double kLockShare = 0.2;              // ...and its share of the call
constexpr int64_t kChurnEvaluations = 20;       // a subscription evaluated this often...
constexpr double kChurnChanged = 0.1;           // ...whose answer changed in at most this share

int histogram_bucket(int64_t us) {
  if (us <= 32) return 0;
  int b = int(std::ceil(2.0 * std::log2(double(us) / 32.0)));
  return std::clamp(b, 0, PerfStats::kHistogram - 1);
}

int64_t bucket_top_us(int b) { return int64_t(32.0 * std::pow(2.0, b / 2.0)); }

int64_t percentile(const std::array<int64_t, PerfStats::kHistogram>& hist, int64_t n, double p) {
  if (n == 0) return 0;
  int64_t want = std::max<int64_t>(1, int64_t(std::ceil(p * double(n))));
  int64_t seen = 0;
  for (int b = 0; b < PerfStats::kHistogram; ++b) {
    seen += hist[b];
    if (seen >= want) return bucket_top_us(b);
  }
  return bucket_top_us(PerfStats::kHistogram - 1);
}

std::string key3(const std::string& a, const std::string& b, const std::string& c) { return a + '\x1f' + b + '\x1f' + c; }

bj::array strings(const std::vector<std::string>& v) {
  bj::array a;
  for (const auto& s : v) a.emplace_back(s);
  return a;
}

int64_t window_ms(const std::string& w) {
  if (w == "5m") return 5 * kMinuteMs;
  if (w == "15m") return 15 * kMinuteMs;
  return kHourMs;
}

}  // namespace

bj::object CallPerf::to_json() const {
  bj::object o{{"box_us", box_us}, {"sql_us", sql_us}, {"out_us", out_us}, {"statements", statements}};
  if (queue_us) o["queue_us"] = queue_us;
  if (lock_us) o["lock_us"] = lock_us;
  if (statement_us) o["statement_us"] = statement_us;
  if (commit_us) o["commit_us"] = commit_us;
  if (render_us) o["render_us"] = render_us;
  if (bytes_out) o["bytes_out"] = bytes_out;
  if (refused_writes) o["refused_writes"] = refused_writes;
  if (!cache.empty()) o["cache"] = cache;
  return o;
}

void PerfStats::Bucket::add(const Bucket& o) {
  calls += o.calls;
  failed += o.failed;
  queue_us += o.queue_us;
  total_us += o.total_us;
  box_us += o.box_us;
  sql_us += o.sql_us;
  lock_us += o.lock_us;
  statement_us += o.statement_us;
  commit_us += o.commit_us;
  out_us += o.out_us;
  render_us += o.render_us;
  bytes_out += o.bytes_out;
  statements += o.statements;
  refused_writes += o.refused_writes;
  cache_hit += o.cache_hit;
  cache_miss += o.cache_miss;
  cache_stale += o.cache_stale;
  cache_304 += o.cache_304;
  cache_check += o.cache_check;
  cache_mismatch += o.cache_mismatch;
  evaluations += o.evaluations;
  changed += o.changed;
  for (int i = 0; i < kHistogram; ++i) hist[i] += o.hist[i];
}

PerfStats::Slot PerfStats::Statement::sum(int64_t now_ms) const {
  Slot s;
  for (const auto& [slot, v] : slots) {
    if (slot * kSlotMs < now_ms - kHourMs) continue;
    s.count += v.count;
    s.total_us += v.total_us;
    s.max_us = std::max(s.max_us, v.max_us);
    s.rows += v.rows;
    s.vm_steps += v.vm_steps;
    s.fullscan_steps += v.fullscan_steps;
    s.sorts += v.sorts;
    s.autoindexes += v.autoindexes;
  }
  return s;
}

std::string PerfStats::normalize_sql(std::string_view sql) {
  std::string out;
  bool space = false;
  for (char c : sql) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += c;
  }
  if (out.size() > 2000) out = out.substr(0, 2000) + "…";
  return out;
}

std::vector<std::string> PerfStats::index_columns(std::string_view sql, std::string* table) {
  // Words, with quotes and brackets taken off names and operators kept as their own words.
  std::vector<std::string> words;
  for (size_t i = 0; i < sql.size();) {
    char c = sql[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
    } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '"' || c == '`' || c == '[') {
      std::string w;
      while (i < sql.size()) {
        char d = sql[i];
        if (d == '"' || d == '`' || d == '[' || d == ']') {
          ++i;
          continue;
        }
        if (!(std::isalnum(static_cast<unsigned char>(d)) || d == '_' || d == '.')) break;
        w += d;
        ++i;
      }
      words.push_back(std::move(w));
    } else if (c == '\'') {
      ++i;
      while (i < sql.size() && sql[i] != '\'') ++i;
      ++i;
      words.emplace_back("'");
    } else {
      std::string op(1, c);
      if (i + 1 < sql.size() && (sql[i + 1] == '=' || (c == '<' && sql[i + 1] == '>'))) op += sql[++i];
      ++i;
      words.push_back(op);
    }
  }
  auto upper = [](std::string s) {
    for (auto& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
  };
  auto bare = [](std::string s) {
    auto dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(dot + 1);
  };
  static const std::set<std::string> kOps = {"=", "==", "<", ">", "<=", ">=", "!=", "<>", "IN", "LIKE", "IS", "BETWEEN", "GLOB"};
  std::vector<std::string> cols;
  auto add = [&](const std::string& c) {
    if (c.empty() || std::isdigit(static_cast<unsigned char>(c[0]))) return;
    if (std::find(cols.begin(), cols.end(), c) == cols.end() && cols.size() < 4) cols.push_back(c);
  };
  std::string mode;
  for (size_t k = 0; k < words.size(); ++k) {
    std::string u = upper(words[k]);
    if (u == "FROM" || u == "UPDATE" || (u == "INTO" && k > 0)) {
      // A subquery's "FROM (" names no table; the one inside it does.
      if (table && table->empty() && k + 1 < words.size() && !words[k + 1].empty() &&
          (std::isalpha(static_cast<unsigned char>(words[k + 1][0])) || words[k + 1][0] == '_')) {
        *table = bare(words[k + 1]);
      }
      continue;
    }
    if (u == "WHERE") {
      mode = "where";
      continue;
    }
    if (u == "ORDER" && k + 1 < words.size() && upper(words[k + 1]) == "BY") {
      mode = "order";
      ++k;
      continue;
    }
    if (u == "GROUP" || u == "LIMIT" || u == "HAVING" || u == "RETURNING" || u == "UNION") {
      mode.clear();
      continue;
    }
    if (mode == "where" && k + 1 < words.size() && kOps.count(upper(words[k + 1])) && !words[k].empty() &&
        (std::isalpha(static_cast<unsigned char>(words[k][0])) || words[k][0] == '_')) {
      static const std::set<std::string> kKeywords = {"AND", "OR", "NOT", "NULL"};
      if (!kKeywords.count(u)) add(bare(words[k]));
    } else if (mode == "order" && !words[k].empty() &&
               (std::isalpha(static_cast<unsigned char>(words[k][0])) || words[k][0] == '_') && u != "ASC" &&
               u != "DESC" && u != "NULLS" && u != "FIRST" && u != "LAST") {
      add(bare(words[k]));
    }
  }
  return cols;
}

PerfStats::Route& PerfStats::route_for(const std::string& tenant, const std::string& box, const std::string& route,
                                       const std::string& verb) {
  auto& r = routes_[key3(tenant, box, route)];
  if (r.box.empty()) {
    r.tenant = tenant;
    r.box = box;
    r.route = route;
  }
  if (!verb.empty()) r.verb = verb;
  return r;
}

void PerfStats::prune(int64_t now_ms) {
  if (now_ms - pruned_ms_ < kMinuteMs) return;
  pruned_ms_ = now_ms;
  int64_t oldest = (now_ms - kHourMs) / kMinuteMs;
  for (auto it = routes_.begin(); it != routes_.end();) {
    auto& m = it->second.minutes;
    m.erase(m.begin(), m.lower_bound(oldest));
    it = m.empty() ? routes_.erase(it) : std::next(it);
  }
  for (auto it = writes_.begin(); it != writes_.end();) {
    it->second.erase(it->second.begin(), it->second.lower_bound(oldest));
    it = it->second.empty() ? writes_.erase(it) : std::next(it);
  }
  for (auto it = statements_.begin(); it != statements_.end();) {
    it = it->second.last_ms < now_ms - kHourMs ? statements_.erase(it) : std::next(it);
  }
}

void PerfStats::record_call(const CallPerf& c, int64_t now_ms) {
  std::lock_guard lock(mu_);
  prune(now_ms);
  auto& r = route_for(c.tenant, c.box, c.route, c.verb);
  auto& b = r.minutes[now_ms / kMinuteMs];
  ++b.calls;
  b.failed += c.failed ? 1 : 0;
  b.queue_us += c.queue_us;
  b.total_us += c.total_us;
  b.box_us += c.box_us;
  b.sql_us += c.sql_us;
  b.lock_us += c.lock_us;
  b.statement_us += c.statement_us;
  b.commit_us += c.commit_us;
  b.out_us += c.out_us;
  b.render_us += c.render_us;
  b.bytes_out += c.bytes_out;
  b.statements += c.statements;
  b.refused_writes += c.refused_writes;
  ++b.hist[histogram_bucket(c.total_us)];
  if (c.cache == "hit") ++b.cache_hit;
  else if (c.cache == "miss") ++b.cache_miss;
  else if (c.cache == "stale") ++b.cache_stale;
  else if (c.cache == "304") ++b.cache_304;
  else if (c.cache == "check") ++b.cache_check;
  if (!c.databases_read.empty()) r.databases_read = c.databases_read;
  for (const auto& d : c.databases_written) ++writes_[d][now_ms / kMinuteMs];
}

void PerfStats::record_statements(const std::string& box, const std::string& route,
                                  const std::vector<db::StatementRecord>& done, int64_t now_ms) {
  if (done.empty()) return;
  std::lock_guard lock(mu_);
  prune(now_ms);
  for (const auto& s : done) {
    std::string text = normalize_sql(s.sql);
    std::string key = s.tenant + '\x1f' + box + '\x1f' + s.strategy + '\x1f' + text;
    auto it = statements_.find(key);
    if (it == statements_.end()) {
      // Bounded: past the cap, the statement run longest ago makes room.
      if (statements_.size() >= kMaxStatements) {
        auto oldest = std::min_element(statements_.begin(), statements_.end(),
                                       [](const auto& a, const auto& b) { return a.second.last_ms < b.second.last_ms; });
        statements_.erase(oldest);
      }
      it = statements_.emplace(key, Statement{s.tenant, box, s.strategy, s.database, text, {}, {}, 0}).first;
    }
    auto& st = it->second;
    st.last_ms = now_ms;
    if (std::find(st.routes.begin(), st.routes.end(), route) == st.routes.end() && st.routes.size() < 8) {
      st.routes.push_back(route);
    }
    auto& slot = st.slots[now_ms / kSlotMs];
    ++slot.count;
    slot.total_us += s.us;
    slot.max_us = std::max(slot.max_us, s.us);
    slot.rows += s.rows;
    slot.vm_steps += s.vm_steps;
    slot.fullscan_steps += s.fullscan_steps;
    slot.sorts += s.sorts;
    slot.autoindexes += s.autoindexes;
    while (st.slots.size() > 6) st.slots.erase(st.slots.begin());
  }
}

void PerfStats::record_subscription(const std::string& tenant, const std::string& box, const std::string& route,
                                    bool changed, int64_t now_ms) {
  std::lock_guard lock(mu_);
  prune(now_ms);
  auto& b = route_for(tenant, box, route, "subscribe").minutes[now_ms / kMinuteMs];
  ++b.evaluations;
  b.changed += changed ? 1 : 0;
}

void PerfStats::record_cache(const std::string& tenant, const std::string& box, const std::string& route,
                             const std::string& answer, int64_t now_ms) {
  std::lock_guard lock(mu_);
  prune(now_ms);
  auto& b = route_for(tenant, box, route, "").minutes[now_ms / kMinuteMs];
  if (answer == "hit") ++b.cache_hit;
  else if (answer == "miss") ++b.cache_miss;
  else if (answer == "stale") ++b.cache_stale;
  else if (answer == "304") ++b.cache_304;
  else if (answer == "check") ++b.cache_check;
  else if (answer == "mismatch") ++b.cache_mismatch;
}

bj::array PerfStats::statements(const std::string& tenant, const std::string& box, const std::string& sort, int limit,
                                int64_t now_ms) {
  std::lock_guard lock(mu_);
  prune(now_ms);
  std::vector<std::pair<const Statement*, Slot>> rows;
  for (const auto& [k, st] : statements_) {
    if ((!tenant.empty() && st.tenant != tenant) || (!box.empty() && st.box != box)) continue;
    Slot s = st.sum(now_ms);
    if (s.count) rows.emplace_back(&st, s);
  }
  auto by = [&](const Slot& s) {
    if (sort == "count") return s.count;
    if (sort == "fullscan") return s.fullscan_steps;
    if (sort == "max") return s.max_us;
    return s.total_us;
  };
  std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return by(a.second) > by(b.second); });
  bj::array out;
  for (const auto& [st, s] : rows) {
    if (int(out.size()) >= limit) break;
    out.push_back(bj::object{{"tenant", st->tenant},
                             {"box", st->box},
                             {"strategy", st->strategy},
                             {"database", st->database},
                             {"sql", st->sql},
                             {"routes", strings(st->routes)},
                             {"count", s.count},
                             {"total_us", s.total_us},
                             {"avg_us", s.total_us / s.count},
                             {"max_us", s.max_us},
                             {"rows", s.rows},
                             {"vm_steps", s.vm_steps},
                             {"fullscan_steps", s.fullscan_steps},
                             {"sorts", s.sorts},
                             {"autoindexes", s.autoindexes}});
  }
  return out;
}

bj::object PerfStats::report(const std::string& tenant, const std::string& box, int64_t window, int64_t now_ms) {
  std::lock_guard lock(mu_);
  prune(now_ms);
  int64_t from = (now_ms - window) / kMinuteMs;
  bj::array routes, findings;
  auto finding = [&](const char* kind, const std::string& t, const std::string& b, const std::string& r,
                     std::string text, bj::object evidence) {
    bj::object f{{"kind", kind}, {"tenant", t}, {"box", b}, {"text", std::move(text)}, {"evidence", std::move(evidence)}};
    if (!r.empty()) f["route"] = r;
    findings.push_back(std::move(f));
  };
  for (const auto& [k, r] : routes_) {
    if ((!tenant.empty() && r.tenant != tenant) || (!box.empty() && r.box != box)) continue;
    Bucket s;
    for (auto it = r.minutes.lower_bound(from); it != r.minutes.end(); ++it) s.add(it->second);
    if (!s.calls && !s.evaluations && !s.cache_mismatch) continue;
    int64_t n = std::max<int64_t>(s.calls, 1);
    bj::object avg{{"queue_us", s.queue_us / n},        {"total_us", s.total_us / n}, {"box_us", s.box_us / n},
                   {"sql_us", s.sql_us / n},            {"lock_us", s.lock_us / n},   {"statement_us", s.statement_us / n},
                   {"commit_us", s.commit_us / n},      {"out_us", s.out_us / n},     {"render_us", s.render_us / n},
                   {"bytes_out", s.bytes_out / n},      {"statements", double(s.statements) / double(n)}};
    int64_t cache_answers = s.cache_hit + s.cache_miss + s.cache_stale + s.cache_304;
    bj::object cache{{"hit", s.cache_hit}, {"miss", s.cache_miss}, {"stale", s.cache_stale},
                     {"not_modified", s.cache_304}, {"check", s.cache_check}, {"mismatch", s.cache_mismatch}};
    cache["hit_rate"] = cache_answers ? double(s.cache_hit + s.cache_304) / double(cache_answers) : 0.0;
    // The route's three costliest statements.
    std::vector<std::pair<const Statement*, Slot>> top;
    for (const auto& [sk, st] : statements_) {
      if (st.tenant != r.tenant || st.box != r.box ||
          std::find(st.routes.begin(), st.routes.end(), r.route) == st.routes.end()) {
        continue;
      }
      Slot sum = st.sum(now_ms);
      if (sum.count) top.emplace_back(&st, sum);
    }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.total_us > b.second.total_us; });
    bj::array costly;
    for (size_t i = 0; i < top.size() && i < 3; ++i) {
      costly.push_back(bj::object{{"sql", top[i].first->sql},
                                  {"count", top[i].second.count},
                                  {"total_us", top[i].second.total_us},
                                  {"fullscan_steps", top[i].second.fullscan_steps}});
    }
    bj::object row{{"tenant", r.tenant}, {"box", r.box},           {"route", r.route},     {"verb", r.verb},
                   {"calls", s.calls},   {"failed", s.failed},     {"p50_us", percentile(s.hist, s.calls, 0.50)},
                   {"p95_us", percentile(s.hist, s.calls, 0.95)},  {"p99_us", percentile(s.hist, s.calls, 0.99)},
                   {"average", std::move(avg)},                    {"refused_writes", s.refused_writes},
                   {"cache", std::move(cache)},                    {"costliest_statements", std::move(costly)}};
    if (s.evaluations) row["subscriptions"] = bj::object{{"evaluations", s.evaluations}, {"changed", s.changed}};
    routes.push_back(std::move(row));

    // Findings, each with its evidence.
    if (r.verb == "get" && s.calls >= kCacheableCalls && !s.refused_writes && !cache_answers && !s.cache_check &&
        !r.databases_read.empty()) {
      int64_t writes = 0;
      for (const auto& d : r.databases_read) {
        if (auto w = writes_.find(d); w != writes_.end()) {
          for (auto it = w->second.lower_bound(from); it != w->second.end(); ++it) writes += it->second;
        }
      }
      if (writes * kCacheableWritesPer <= s.calls) {
        finding("cacheable", r.tenant, r.box, r.route,
                "a GET read " + std::to_string(s.calls) + " times while its databases changed " + std::to_string(writes) +
                    " times: declaring \"cache\" on the route would answer most of these without the box",
                bj::object{{"calls", s.calls}, {"database_writes", writes}, {"databases", strings(r.databases_read)}});
      }
    }
    if (s.calls && s.lock_us / n >= kLockWaitUs && double(s.lock_us) >= kLockShare * double(s.total_us)) {
      finding("lock_wait", r.tenant, r.box, r.route,
              "waits " + std::to_string(s.lock_us / n / 1000) + " ms a call for a database's write lock: keep writes " +
                  "short, split hot tables into their own database, or make reads GETs, which never wait",
              bj::object{{"calls", s.calls}, {"lock_us_avg", s.lock_us / n}, {"total_us_avg", s.total_us / n}});
    }
    if (s.refused_writes) {
      finding("get_writes", r.tenant, r.box, r.route,
              "tried to write " + std::to_string(s.refused_writes) + " times as a read (a GET, a HEAD, or a subscription): "
              "move the write to a POST",
              bj::object{{"refused_writes", s.refused_writes}, {"calls", s.calls}});
    }
    if (s.evaluations >= kChurnEvaluations && double(s.changed) <= kChurnChanged * double(s.evaluations)) {
      finding("subscription_churn", r.tenant, r.box, r.route,
              "evaluated " + std::to_string(s.evaluations) + " times; its answer changed " + std::to_string(s.changed) +
                  ": its wake topics move far more often than its answer, so narrow them",
              bj::object{{"evaluations", s.evaluations}, {"changed", s.changed}});
    }
    if (s.cache_mismatch) {
      finding("cache_mismatch", r.tenant, r.box, r.route,
              std::to_string(s.cache_mismatch) + " sampled checks found the box's answer differs from the cached one: it "
              "depends on more than its cache declaration says (the clock, randomness, or a topic missing from wake)",
              bj::object{{"mismatches", s.cache_mismatch}, {"checks", s.cache_check}});
    }
  }
  for (const auto& [k, st] : statements_) {
    if ((!tenant.empty() && st.tenant != tenant) || (!box.empty() && st.box != box)) continue;
    Slot s = st.sum(now_ms);
    if (s.count < kScanRuns) continue;
    if (s.fullscan_steps / s.count < kScanStepsPerRun && !s.autoindexes) continue;
    std::string table;
    auto cols = index_columns(st.sql, &table);
    std::string text = "scans " + (table.empty() ? std::string("its table") : table) + " (" +
                       std::to_string(s.fullscan_steps / s.count) + " rows a run" +
                       (s.autoindexes ? ", and SQLite built a temporary index" : "") + ")";
    bj::object evidence{{"sql", st.sql}, {"count", s.count}, {"fullscan_steps", s.fullscan_steps},
                        {"autoindexes", s.autoindexes}, {"database", st.database}};
    if (!table.empty() && !cols.empty()) {
      std::string name = table + "_by";
      for (const auto& c : cols) name += "_" + c;
      text += ": add an index on " + table + " (";
      for (size_t i = 0; i < cols.size(); ++i) text += (i ? ", " : "") + cols[i];
      text += ") in a new schema version";
      evidence["suggested_index"] = bj::object{{"table", table}, {"name", name.substr(0, 63)}, {"columns", strings(cols)}};
    } else {
      text += ": index the columns its WHERE and ORDER BY use";
    }
    finding("missing_index", st.tenant, st.box, st.routes.empty() ? "" : st.routes.front(), text, std::move(evidence));
  }
  return bj::object{{"window_ms", window}, {"routes", std::move(routes)}, {"findings", std::move(findings)}};
}

void Host::register_perf_tools() {
  auto tenant_prop = mcp::string_property(
      "A tenant, like example. Required for a focused operator; left out, every tenant.");
  auto box_prop = mcp::string_property("Only this box (as box_list names it).");
  mcp_->add_tool(
      {"perf_report",
       "Read tool. Where each route's time went, in memory for the last hour (design/machine/037.Caching.md § 7): per "
       "route, calls, failures, p50/p95/p99 (µs), the average split (queue_us waiting for a box thread, box_us the "
       "box's own work, sql_us with lock_us waiting for the write lock, statement_us, commit_us, out_us calls out, "
       "render_us templates, bytes_out), refused writes (a GET that tried to write), the cache's answers, subscription "
       "evaluations, and its three costliest statements. Then findings, each with its evidence: cacheable (a GET whose "
       "databases rarely change), missing_index (a statement that scans, with the index to add), lock_wait, get_writes, "
       "subscription_churn, cache_mismatch, browser_cache (a route whose answers browsers may keep). Counted since the host started, an hour at most. perf_statements lists "
       "every statement.",
       mcp::object_schema({{"tenant", tenant_prop},
                           {"box", box_prop},
                           {"window", mcp::string_property("5m, 15m, or 1h (the default).")}},
                          {}),
       [this](const bj::object& args) {
         auto text = [&](const char* k) {
           const auto* v = args.if_contains(k);
           return v && v->is_string() ? std::string(v->as_string()) : std::string();
         };
         auto out = perf_->report(text("tenant"), text("box"), window_ms(text("window")), base::now_unix_ms());
         // Routes that let browsers keep answers (037 § 9.4): the one staleness the host can't
         // take back once sent, so every such route is listed, whatever its numbers.
         auto& findings = out["findings"].as_array();
         for (const auto& [name, box] : boxes_->active()->boxes) {
           if (!text("box").empty() && name != text("box")) continue;
           for (const auto& r : box->manifest.routes) {
             if (!r.cache.on || r.cache.browser_ms <= 0) continue;
             findings.push_back(bj::object{
                 {"kind", "browser_cache"}, {"box", name}, {"route", r.name}, {"browser_ms", r.cache.browser_ms},
                 {"text", "browsers and proxies may keep this route's answers for " + std::to_string(r.cache.browser_ms / 1000) +
                              " s after any change; the host can't take them back. Use browser-ms 0 (they always ask, "
                              "and a 304 is cheap) unless being stale that long is fine"}});
           }
         }
         return ToolResult::ok(std::move(out));
       }});
  mcp_->add_tool(
      {"perf_statements",
       "Read tool. The SQL statements boxes ran in the last hour, one row per tenant, box, strategy, and statement text "
       "(whitespace folded): count, total_us, avg_us, max_us, rows returned, and SQLite's own counters (vm_steps, "
       "fullscan_steps, sorts, autoindexes; a high fullscan_steps per run, or any autoindexes, means an index is "
       "missing). The routes that ran each are listed. At most 2,000 statements are kept; the one run longest ago "
       "makes room.",
       mcp::object_schema({{"tenant", tenant_prop},
                           {"box", box_prop},
                           {"sort", mcp::string_property("total (time, the default), count, fullscan, or max.")},
                           {"limit", mcp::integer_property("Rows to return, 1 to 500 (default 50).")}},
                          {}),
       [this](const bj::object& args) {
         auto text = [&](const char* k) {
           const auto* v = args.if_contains(k);
           return v && v->is_string() ? std::string(v->as_string()) : std::string();
         };
         int64_t limit = 50;
         if (const auto* l = args.if_contains("limit"); l && l->is_int64()) limit = std::clamp<int64_t>(l->as_int64(), 1, 500);
         return ToolResult::ok(
             perf_->statements(text("tenant"), text("box"), text("sort"), int(limit), base::now_unix_ms()));
       }});
}

}  // namespace goober::host
