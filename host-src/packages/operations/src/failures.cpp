#include "goober/operations/failures.h"

#include <algorithm>
#include <optional>
#include <set>

#include "goober/sqlite/sqlite.h"

namespace goober::operations {

namespace fs = std::filesystem;

namespace {

constexpr int64_t kMinuteMs = 60 * 1000;
constexpr int64_t kHourMs = 60 * kMinuteMs;
constexpr int64_t kMinutesKeptMs = 48 * kHourMs;
constexpr int64_t kHoursKeptMs = 400 * 24 * kHourMs;
constexpr int64_t kEventsKeptMs = 30 * 24 * kHourMs;
constexpr int64_t kEventsPerBox = 200;
constexpr size_t kPendingEvents = 1000;  // between flushes; past it, counted but not kept
constexpr size_t kMessageBytes = 300;

struct Window {
  const char* table;
  int64_t span_ms, step_ms;
  static std::optional<Window> of(std::string_view w) {
    if (w == "1h") return Window{"minutes", kHourMs, kMinuteMs};
    if (w == "12h") return Window{"minutes", 12 * kHourMs, 10 * kMinuteMs};
    if (w == "7d") return Window{"hours", 7 * 24 * kHourMs, kHourMs};
    if (w == "30d") return Window{"hours", 30 * 24 * kHourMs, 6 * kHourMs};
    return std::nullopt;
  }
  int64_t to(int64_t now) const { return (now / step_ms + 1) * step_ms; }
  int64_t from(int64_t now) const { return to(now) - span_ms; }
};

// Cut at a character boundary, never inside one.
std::string clip(std::string s) {
  if (s.size() <= kMessageBytes) return s;
  size_t n = kMessageBytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  s.resize(n);
  return s + "…";
}

}  // namespace

std::string_view Failures::group_of(std::string_view cls) {
  static const std::map<std::string_view, std::string_view> kGroups = {
      {"trap", "call"},           {"timeout", "call"},          {"out_of_fuel", "call"},    {"failed", "call"},
      {"no_response", "call"},    {"bad_response", "call"},     {"bad_status", "call"},     {"outbox_failed", "call"},
      {"render_failed", "call"},
      {"host_write_failed", "call"}, {"commit_failed", "call"}, {"host_error", "call"},     {"sql_error", "sql"},
      {"no_clock", "sql"},        {"refused_write", "sql"},     {"delivery_retry", "outbox"}, {"delivery_dead", "outbox"},
      {"internal_failed", "internal"}, {"mail_rejected", "answer"}, {"mail_deferred", "answer"}};
  auto it = kGroups.find(cls);
  return it == kGroups.end() ? std::string_view() : it->second;
}

Failures::Failures(fs::path db_file) {
  fs::create_directories(db_file.parent_path());
  db_ = std::make_unique<sqlite::Db>(db_file);
  db_->exec("PRAGMA synchronous=NORMAL;");
  for (const char* table : {"minutes", "hours"}) {
    db_->exec(std::string("CREATE TABLE IF NOT EXISTS ") + table +
              " (t INTEGER NOT NULL, tenant TEXT NOT NULL, box TEXT NOT NULL, route TEXT NOT NULL, kind TEXT NOT NULL,"
              " class TEXT NOT NULL, n INTEGER NOT NULL, PRIMARY KEY (t, tenant, box, route, kind, class))");
  }
  db_->exec(
      "CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY, at_ms INTEGER NOT NULL, tenant TEXT NOT NULL,"
      " box TEXT NOT NULL, version TEXT NOT NULL, route TEXT NOT NULL, kind TEXT NOT NULL, class TEXT NOT NULL,"
      " message TEXT NOT NULL, request_id TEXT NOT NULL, n INTEGER NOT NULL)");
  db_->exec("CREATE INDEX IF NOT EXISTS events_by_box ON events (box, id)");
}

Failures::~Failures() = default;

void Failures::add(int64_t at_ms, const std::string& tenant, const std::string& box, const std::string& route,
                   const std::string& kind, const std::string& cls, int64_t n) {
  int64_t minute = at_ms / kMinuteMs * kMinuteMs;
  int64_t hour = at_ms / kHourMs * kHourMs;
  std::lock_guard lock(pending_mu_);
  pending_[{minute, tenant, box, route, kind, cls}] += n;
  pending_[{-hour - 1, tenant, box, route, kind, cls}] += n;
}

void Failures::call(int64_t at_ms, const std::string& tenant, const std::string& box, const std::string& route,
                    const std::string& kind) {
  add(at_ms, tenant, box, route, kind, "", 1);
}

void Failures::failure(Event e) {
  if (e.n <= 0) return;
  add(e.at_ms, e.tenant, e.box, e.route, e.kind, e.cls, e.n);
  e.message = clip(std::move(e.message));
  std::lock_guard lock(pending_mu_);
  if (pending_events_.size() < kPendingEvents) {
    pending_events_.push_back(std::move(e));
  } else {
    ++dropped_events_;
  }
}

void Failures::flush(int64_t now_ms) {
  std::map<Key, int64_t> counts;
  std::vector<Event> events;
  {
    std::lock_guard lock(pending_mu_);
    counts.swap(pending_);
    events.swap(pending_events_);
    dropped_events_ = 0;
  }
  std::lock_guard lock(db_mu_);
  if (!counts.empty() || !events.empty()) {
    db_->exec("BEGIN IMMEDIATE");
    try {
      std::set<std::string> boxes;
      for (const auto& [k, n] : counts) {
        auto [t, tenant, box, route, kind, cls] = k;
        sqlite::Stmt w(*db_, std::string("INSERT INTO ") + (t < 0 ? "hours" : "minutes") +
                                 " VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT (t, tenant, box, route, kind, class) DO "
                                 "UPDATE SET n = n + excluded.n");
        w.bind(1, t < 0 ? -t - 1 : t).bind(2, tenant).bind(3, box).bind(4, route).bind(5, kind).bind(6, cls).bind(7, n);
        w.run();
      }
      for (const auto& e : events) {
        sqlite::Stmt w(*db_,
                       "INSERT INTO events (at_ms, tenant, box, version, route, kind, class, message, request_id, n) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        w.bind(1, e.at_ms).bind(2, e.tenant).bind(3, e.box).bind(4, e.version).bind(5, e.route).bind(6, e.kind);
        w.bind(7, e.cls).bind(8, e.message).bind(9, e.request_id).bind(10, e.n);
        w.run();
        boxes.insert(e.box);
      }
      // The newest 200 per box are kept.
      for (const auto& box : boxes) {
        sqlite::Stmt d(*db_,
                       "DELETE FROM events WHERE box = ? AND id <= (SELECT id FROM events WHERE box = ? ORDER BY id DESC "
                       "LIMIT 1 OFFSET ?)");
        d.bind(1, box).bind(2, box).bind(3, kEventsPerBox);
        d.run();
      }
      db_->exec("COMMIT");
    } catch (...) {
      db_->exec("ROLLBACK");
      throw;
    }
  }
  if (now_ms - last_drop_ms_ > 10 * kMinuteMs) {
    last_drop_ms_ = now_ms;
    for (auto [sql, keep] : {std::pair{"DELETE FROM minutes WHERE t < ?", kMinutesKeptMs},
                             std::pair{"DELETE FROM hours WHERE t < ?", kHoursKeptMs},
                             std::pair{"DELETE FROM events WHERE at_ms < ?", kEventsKeptMs}}) {
      sqlite::Stmt s(*db_, sql);
      s.bind(1, now_ms - keep);
      s.run();
    }
  }
}

bj::object Failures::report(std::string_view window, const std::string& tenant, const std::string& box, int64_t now_ms,
                            size_t latest) {
  auto w = Window::of(window).value_or(*Window::of("12h"));
  std::lock_guard lock(db_mu_);
  int64_t from = w.from(now_ms), to = w.to(now_ms);
  size_t count = size_t(w.span_ms / w.step_ms);
  struct Point {
    int64_t calls = 0, call = 0, sql = 0, outbox = 0, internal = 0, answer = 0;
  };
  std::vector<Point> points(count);
  struct Route {
    int64_t calls = 0, failures = 0;
    std::map<std::string, int64_t> classes;
  };
  std::map<std::tuple<std::string, std::string, std::string, std::string>, Route> routes;
  std::map<std::string, int64_t> classes;
  int64_t calls = 0, failures = 0;
  sqlite::Stmt s(*db_, std::string("SELECT t, tenant, box, route, kind, class, n FROM ") + w.table +
                           " WHERE t >= ? AND t < ? AND (? = '' OR tenant = ?) AND (? = '' OR box = ?)");
  s.bind(1, from).bind(2, to).bind(3, tenant).bind(4, tenant).bind(5, box).bind(6, box);
  while (s.step()) {
    size_t i = size_t((s.int64(0) - from) / w.step_ms);
    std::string cls = s.text(5);
    int64_t n = s.int64(6);
    auto& r = routes[{s.text(1), s.text(2), s.text(3), s.text(4)}];
    if (cls.empty()) {
      r.calls += n;
      calls += n;
      if (i < count) points[i].calls += n;
      continue;
    }
    r.classes[cls] += n;
    classes[cls] += n;
    std::string_view group = group_of(cls);
    if (group == "call") {
      r.failures += n;
      failures += n;
    }
    if (i < count) {
      auto& p = points[i];
      (group == "call" ? p.call : group == "sql" ? p.sql : group == "outbox" ? p.outbox : group == "internal" ? p.internal
                                                                                                            : p.answer) += n;
    }
  }
  bj::array pts;
  for (size_t i = 0; i < count; ++i) {
    const auto& p = points[i];
    pts.push_back(bj::object{{"t", from + int64_t(i) * w.step_ms}, {"calls", p.calls}, {"failures", p.call},
                             {"sql", p.sql}, {"outbox", p.outbox}, {"internal", p.internal}, {"answer", p.answer}});
  }
  std::vector<std::pair<decltype(routes)::key_type, Route>> list(routes.begin(), routes.end());
  std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
    int64_t fa = a.second.failures, fb = b.second.failures;
    int64_t oa = 0, ob = 0;
    for (const auto& [c, n] : a.second.classes) oa += n;
    for (const auto& [c, n] : b.second.classes) ob += n;
    if (fa != fb) return fa > fb;
    if (oa != ob) return oa > ob;
    return a.second.calls > b.second.calls;
  });
  bj::array rows;
  for (const auto& [k, r] : list) {
    bj::object cls;
    for (const auto& [c, n] : r.classes) cls[c] = n;
    rows.push_back(bj::object{{"tenant", std::get<0>(k)}, {"box", std::get<1>(k)}, {"route", std::get<2>(k)},
                              {"kind", std::get<3>(k)}, {"calls", r.calls}, {"failures", r.failures},
                              {"classes", std::move(cls)}});
  }
  bj::array events;
  {
    sqlite::Stmt e(*db_,
                   "SELECT at_ms, tenant, box, version, route, kind, class, message, request_id, n FROM events WHERE "
                   "at_ms >= ? AND (? = '' OR tenant = ?) AND (? = '' OR box = ?) ORDER BY id DESC LIMIT ?");
    e.bind(1, from).bind(2, tenant).bind(3, tenant).bind(4, box).bind(5, box).bind(6, int64_t(latest));
    while (e.step()) {
      events.push_back(bj::object{{"at_ms", e.int64(0)},  {"tenant", e.text(1)},  {"box", e.text(2)},
                                  {"version", e.text(3)}, {"route", e.text(4)},   {"kind", e.text(5)},
                                  {"class", e.text(6)},   {"message", e.text(7)}, {"request_id", e.text(8)},
                                  {"n", e.int64(9)}});
    }
  }
  bj::object totals_classes;
  for (const auto& [c, n] : classes) totals_classes[c] = n;
  bj::object groups;
  for (const char* c : {"trap", "timeout", "out_of_fuel", "failed", "no_response", "bad_response", "bad_status",
                        "render_failed", "outbox_failed", "host_write_failed", "commit_failed", "host_error",
                        "sql_error", "no_clock",
                        "refused_write", "delivery_retry", "delivery_dead", "internal_failed", "mail_rejected",
                        "mail_deferred"}) {
    groups[c] = std::string(group_of(c));
  }
  return bj::object{{"window", std::string(window)},
                    {"step_ms", w.step_ms},
                    {"from_ms", from},
                    {"to_ms", to},
                    {"groups", std::move(groups)},
                    {"totals", bj::object{{"calls", calls}, {"failures", failures}, {"classes", std::move(totals_classes)}}},
                    {"points", std::move(pts)},
                    {"routes", std::move(rows)},
                    {"latest", std::move(events)}};
}

}  // namespace goober::operations
