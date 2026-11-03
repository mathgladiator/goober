#include "goober/controls/report.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "goober/controls/gate.h"

namespace goober::controls {

namespace {

constexpr int64_t kHourMs = 60 * 60 * 1000;
constexpr int64_t kKeptMs = 7 * 24 * kHourMs;

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

// A span's rows added up, with the top clients grouped across buckets.
struct Sum {
  operations::ControlRow row;
  std::map<std::string, int64_t> top, would_top;
  void add(const operations::ControlRow& r) {
    row.merge(r);
    if (!r.top_client.empty()) top[r.top_client] += r.top_n;
    if (!r.would_top.empty()) would_top[r.would_top] += r.would_top_n;
  }
  static double share(const std::map<std::string, int64_t>& m, int64_t of) {
    int64_t best = 0;
    for (const auto& kv : m) best = std::max(best, kv.second);
    return of > 0 ? double(best) / double(of) : 0.0;
  }
};

double ratio(int64_t part, int64_t whole) { return whole > 0 ? double(part) / double(whole) : 0.0; }

std::string pct(double share) {
  double p = share * 100;
  char buf[32];
  if (p > 0 && p < 10) std::snprintf(buf, sizeof buf, "%.1f%%", p);
  else std::snprintf(buf, sizeof buf, "%.0f%%", p);
  return buf;
}

bj::object brief(const Sum& s) {
  return bj::object{{"requests", s.row.matched},
                    {"allowed", s.row.allowed},
                    {"refused", s.row.refused()},
                    {"would", s.row.would},
                    {"p99_us", s.row.hist.percentile(0.99)},
                    {"ratio_5xx", ratio(s.row.n5, s.row.allowed)}};
}

bj::value change_json(const Change& c) {
  return bj::object{{"at_ms", c.at_ms}, {"by", c.by}, {"why", c.why}, {"before", c.before}, {"after", c.after}};
}

}  // namespace

bool valid_report_window(std::string_view window) { return Window::of(window).has_value(); }

bj::object report(const ReportInput& in, std::string_view window, int64_t now_ms) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  int64_t from = w.from(now_ms), to = w.to(now_ms);
  size_t n = size_t(w.span_ms / w.step_ms);
  std::vector<Sum> points(n);
  Sum total;
  for (const auto& r : in.rows) {
    if (r.t < from || r.t >= to) continue;
    size_t i = size_t((r.t - from) / w.step_ms);
    if (i >= n) continue;
    points[i].add(r);
    total.add(r);
  }
  bj::array pts;
  for (size_t i = 0; i < n; ++i) {
    const auto& p = points[i].row;
    pts.push_back(bj::object{{"t", from + int64_t(i) * w.step_ms},
                             {"matched", p.matched},
                             {"allowed", p.allowed},
                             {"refused", p.refused()},
                             {"rate", p.rate},
                             {"concurrent", p.concurrent},
                             {"body", p.body},
                             {"closed", p.closed},
                             {"would", p.would},
                             {"n5", p.n5},
                             {"peak", p.peak},
                             {"p50_us", p.hist.percentile(0.5)},
                             {"p99_us", p.hist.percentile(0.99)}});
  }
  const auto& t = total.row;
  int64_t refused = t.refused();
  int64_t clients = sketch_count(t.clients), would_clients = sketch_count(t.would_clients);
  double top_share = Sum::share(total.top, refused), would_top_share = Sum::share(total.would_top, t.would);
  bj::object totals{{"matched", t.matched},
                    {"allowed", t.allowed},
                    {"refused", refused},
                    {"rate", t.rate},
                    {"concurrent", t.concurrent},
                    {"body", t.body},
                    {"closed", t.closed},
                    {"would", t.would},
                    {"refused_ratio", ratio(refused, t.matched)},
                    {"would_ratio", ratio(t.would, t.matched)},
                    {"clients_refused", clients},
                    {"top_client_share", top_share},
                    {"would_clients", would_clients},
                    {"would_top_share", would_top_share},
                    {"peak", t.peak},
                    {"p50_us", t.hist.percentile(0.5)},
                    {"p99_us", t.hist.percentile(0.99)},
                    {"ratio_5xx", ratio(t.n5, t.allowed)}};
  auto many = [](int64_t c) { return c >= kSketchSaturated ? std::string("1000 or more") : std::to_string(c); };

  // Verdicts, most telling first.
  bj::array verdicts;
  std::string win(window);
  auto say = [&](std::string s) { verdicts.emplace_back(s); };
  bool observing = in.mode && *in.mode == Mode::kObserve;
  if (!in.mode) say("removed: its rows stay until they age out");
  if (t.matched == 0) {
    say("never matched in " + win + ": check the match, or nothing called it");
  } else {
    if (t.closed > 0) say("closed: answered " + std::to_string(t.closed) + " requests without waking anything");
    if (refused > 0 && refused >= kShieldMinRefused && top_share >= kShieldShare) {
      say("refusals come mostly from one client (" + pct(top_share) + " of " + std::to_string(refused) + "): shielding");
    }
    bool tight = clients >= kManyClients || (clients >= 3 && ratio(refused - t.closed, t.matched) >= kTightShare);
    if (refused - t.closed > 0 && tight && top_share < kShieldShare) {
      say("refuses many clients (" + many(clients) + ", " + pct(ratio(refused, t.matched)) + " of requests): may be too tight");
    }
    bool said = verdicts.size() > (in.mode ? 0u : 1u) + (t.closed > 0 ? 1u : 0u);
    if (refused - t.closed > 0 && !said) {
      say("refused " + std::to_string(refused - t.closed) + " of " + std::to_string(t.matched) + " (" +
          pct(ratio(refused - t.closed, t.matched)) + ") from " + many(clients) + (clients == 1 ? " client" : " clients") +
          ": too few to judge yet");
    }
    if (!observing && refused == 0 && in.mode) say("matched " + std::to_string(t.matched) + " but never refused (loose or not needed)");
    if (observing || t.would > 0) {
      if (t.would > 0) {
        say("observe: would have refused " + pct(ratio(t.would, t.matched)) + " (" + std::to_string(t.would) + " of " +
            std::to_string(t.matched) + ") from " + many(would_clients) + (would_clients == 1 ? " client" : " clients") +
            (t.would >= kShieldMinRefused && would_top_share >= kShieldShare ? ", mostly one" : ""));
      } else {
        say("observe: would have refused nothing (" + std::to_string(t.matched) + " requests): loose, or safe to enforce");
      }
    }
    if (t.allowed >= k5xxMinAllowed && ratio(t.n5, t.allowed) >= k5xxShare) {
      say(pct(ratio(t.n5, t.allowed)) + " of the allowed answered 5xx: what is behind it is struggling");
    }
  }

  // The effect of the last change: equal spans before and after it.
  bj::value effect = nullptr;
  bj::value last = nullptr;
  bj::array history;
  for (auto it = in.history.rbegin(); it != in.history.rend(); ++it) history.push_back(change_json(*it));
  if (!in.history.empty()) {
    const auto& c = in.history.back();
    last = change_json(c);
    int64_t span = std::min(now_ms - c.at_ms, kKeptMs / 2);
    if (span < kBucketMs) {
      effect = bj::object{{"changed_at_ms", c.at_ms}, {"span_ms", span}, {"note", "changed under five minutes ago: wait for a bucket"}};
    } else {
      Sum before, after;
      int64_t at = c.at_ms / kBucketMs * kBucketMs;
      for (const auto& r : in.rows) {
        if (r.t >= c.at_ms - span && r.t + kBucketMs <= c.at_ms) before.add(r);
        else if (r.t >= at && r.t < c.at_ms + span) after.add(r);
      }
      bj::object e{{"changed_at_ms", c.at_ms}, {"span_ms", span}, {"before", brief(before)}, {"after", brief(after)}};
      if (c.before.is_null()) {
        e["note"] = "the control was created then: nothing was counted under it before";
      } else if (now_ms - c.at_ms > kKeptMs / 2) {
        e["note"] = "the spans are 3.5 days each, the most seven days of rows hold";
      }
      effect = std::move(e);
    }
  }

  bj::object out{{"id", in.id},
                 {"level", in.level},
                 {"mode", in.mode ? bj::value(mode_name(*in.mode)) : bj::value("removed")},
                 {"window", win},
                 {"step_ms", w.step_ms},
                 {"from_ms", from},
                 {"to_ms", to},
                 {"points", std::move(pts)},
                 {"totals", std::move(totals)},
                 {"last_change", std::move(last)},
                 {"history", std::move(history)},
                 {"effect", std::move(effect)},
                 {"verdicts", std::move(verdicts)}};
  return out;
}

}  // namespace goober::controls
