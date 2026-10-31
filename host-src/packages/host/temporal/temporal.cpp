// goober-temporal: the real timer driver against the real clock (design/machine/014.Time.md § 8).
//
// The blocking suite (ctest) drives timers with a simulated clock (Host::timer_tick), so it is
// quick and never waits. This program is the other half: it starts a whole host, lets its timer
// thread run for some minutes on the wall clock, and checks every fire against when it was due.
// It is slow on purpose and runs apart: `just temporal`, and daily (tools/temporal-daily.sh).
//
// It writes one JSON line per expected or actual fire (timer, row, expected, actual, drift_ms,
// verdict) and a final summary line ({"summary": true, "pass": ...}) to
// OUT/temporal-<start>.jsonl, prints the summary, and exits 0 on a pass, 1 on a failure.
//
//   goober-temporal [--minutes N] [--out DIR]     (defaults: 3 minutes, ./temporal-logs)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>

#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/timers/cron.h"

using namespace goober;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr int64_t kEveryMs = 5000;
constexpr int64_t kLateMs = 2000;   // the driver ticks each second; a fire this late is wrong
constexpr int64_t kEarlyMs = 50;    // a fire before its due time is always wrong
constexpr int64_t kClockSkewMs = 1000;  // the wall clock and the monotonic clock may disagree this much

// Each fire writes a tick naming its timer; none responds.
const char* kWat = R"wat((module
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "w")
  (data (i32.const 16) "INSERT INTO ticks (what) VALUES (?)")
  (data (i32.const 64) "[\22beat\22]")
  (data (i32.const 80) "[\22minute\22]")
  (data (i32.const 96) "[\22remind\22]")
  (func $tick (param $p i32) (param $n i32)
    (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 35) (local.get $p) (local.get $n))))
  (func (export "goober_route_beat") (result i32) (call $tick (i32.const 64) (i32.const 8)) (i32.const 0))
  (func (export "goober_route_minute") (result i32) (call $tick (i32.const 80) (i32.const 10)) (i32.const 0))
  (func (export "goober_route_remind") (result i32) (call $tick (i32.const 96) (i32.const 10)) (i32.const 0))
))wat";

const char* kSchema = R"({"schema":"temporal1","tables":{
  "ticks":{"columns":{"what":{"type":"string","max-length":40}}},
  "reminders":{"columns":{"remind_at":{"type":"int64"},"on":{"type":"boolean","default":true}}}}})";

const char* kManifest = R"({"box":"temporal","domains":["*"],
  "data":{"sqlite3":{
    "w":{"schema":"temporal1","database":"temporal","pattern":"sql","tables":{"ticks":{"read":true,"insert":true}}},
    "remind_t":{"schema":"temporal1","database":"temporal","pattern":"by_timer","table":"reminders","active-field":"on"}}},
  "routes":{"timer":{
    "beat":{"name":"beat","every-ms":5000},
    "minute":{"name":"minute","cron":"* * * * *"},
    "remind":{"name":"remind","source":"remind_t","at-field":"remind_at"}}}})";

struct Fire {
  std::string timer, row;
  int64_t due = 0, at = 0;
};

std::string verdict_of(int64_t drift) {
  if (drift < -kEarlyMs) return "early";
  if (drift > kLateMs) return "late";
  return "ok";
}

}  // namespace

int main(int argc, char** argv) {
  int minutes = 3;
  fs::path out_dir = "temporal-logs";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--minutes" && i + 1 < argc) {
      minutes = std::max(1, std::atoi(argv[++i]));
    } else if (a == "--out" && i + 1 < argc) {
      out_dir = argv[++i];
    } else {
      std::cerr << "usage: goober-temporal [--minutes N] [--out DIR]\n";
      return 2;
    }
  }
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  const int64_t wall_start = base::now_unix_ms();
  const auto mono_start = std::chrono::steady_clock::now();
  std::string stamp = base::format_utc_ms(wall_start);
  std::replace(stamp.begin(), stamp.end(), ':', '-');
  fs::path log_path = out_dir / ("temporal-" + stamp + ".jsonl");
  fs::path root = out_dir / ("run-" + stamp);
  fs::remove_all(root, ec);

  // A host of its own, with every loop running on the real clock.
  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 2}})",
                                  root, &err);
  if (!cfg) return std::cerr << "goober-temporal: " << err << "\n", 1;
  int64_t end_ms = wall_start + int64_t(minutes) * 60'000;
  std::vector<int64_t> reminders;
  {
    host::Host host(config::local_mode(cfg));
    if (!host.schemas().put(kSchema, &err) || !host.boxes().upload(kManifest, kWat, &err)) {
      return std::cerr << "goober-temporal: " << err << "\n", 1;
    }
    auto schema = host.schemas().get("temporal1");
    if (!host.databases().open("playground", "temporal", &*schema, &err)) return std::cerr << "goober-temporal: " << err << "\n", 1;
    // One-shot rows spread over the run, each due on its own second.
    for (int64_t at = wall_start + 20'000; at < end_ms - 15'000; at += 45'000) reminders.push_back(at);
    for (int64_t at : reminders) {
      if (!host.databases().operate("playground", "temporal",
                                    "INSERT INTO reminders (remind_at) VALUES (" + std::to_string(at) + ")", nullptr, 1,
                                    true, &err)) {
        return std::cerr << "goober-temporal: " << err << "\n", 1;
      }
    }
    host.start();
    std::cerr << "goober-temporal: running the timer driver for " << minutes << " minute(s); log " << log_path.string()
              << "\n";
    while (base::now_unix_ms() < end_ms) std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }  // the host stops here: every fire has finished and logged
  const int64_t wall_end = base::now_unix_ms();
  const int64_t mono_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - mono_start).count();

  // Every fire, from the host's access log: when it was due, and when it finished.
  std::vector<Fire> fires;
  for (auto it = fs::recursive_directory_iterator(root / "storage" / "logs", ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->path().filename().string().rfind("access", 0) != 0) continue;
    std::ifstream in(it->path());
    std::string line;
    while (std::getline(in, line)) {
      auto v = json::parse(line, &err);
      if (!v || !v->is_object()) continue;
      const auto& o = v->as_object();
      if (!o.contains("method") || !o.at("method").is_string() || o.at("method").as_string() != "timer" || !o.contains("due")) continue;
      Fire f;
      f.timer = std::string(o.at("timer").as_string());
      if (const auto* r = o.if_contains("row")) f.row = r->is_string() ? std::string(r->as_string()) : json::dump(*r);
      f.due = timers::parse_instant_ms(o.at("due").as_string()).value_or(0);
      f.at = timers::parse_instant_ms(o.at("at").as_string()).value_or(0);
      fires.push_back(f);
    }
  }
  std::sort(fires.begin(), fires.end(), [](const Fire& a, const Fire& b) { return a.due < b.due; });

  std::ofstream log(log_path);
  int failures = 0;
  auto write = [&](const std::string& timer, const std::string& row, int64_t expected, std::optional<int64_t> actual,
                   const std::string& verdict) {
    bj::object o{{"timer", timer}, {"expected", base::format_utc_ms(expected)}, {"verdict", verdict}};
    if (!row.empty()) o["row"] = row;
    if (actual) {
      o["actual"] = base::format_utc_ms(*actual);
      o["drift_ms"] = *actual - expected;
    }
    if (verdict != "ok") ++failures;
    log << json::dump(o) << "\n";
  };

  // beat: every 5 seconds, each fire on time, never two in one interval, and none missed.
  int beats = 0;
  int64_t last_beat = 0;
  for (const auto& f : fires) {
    if (f.timer != "beat") continue;
    ++beats;
    std::string v = verdict_of(f.at - f.due);
    if (v == "ok" && last_beat && f.due - last_beat < kEveryMs) v = "too_soon";
    if (v == "ok" && last_beat && f.due - last_beat > kEveryMs + kLateMs) v = "gap";
    write("beat", "", f.due, f.at, v);
    last_beat = f.due;
  }
  int64_t run_ms = wall_end - wall_start;
  int expected_beats = int(run_ms / (kEveryMs + 1000)) - 1;
  if (beats < expected_beats) {
    write("beat", "", wall_end, std::nullopt, "missing");
  }

  // minute: once at every minute boundary inside the run.
  std::string cron_err;
  auto cron = timers::Cron::parse("* * * * *", &cron_err);
  std::map<int64_t, int> minute_fires;
  for (const auto& f : fires) {
    if (f.timer == "minute") ++minute_fires[f.due];
  }
  for (int64_t at = *cron->next(wall_start + 2000, timers::host_zone()); at < wall_end - 3000;
       at = *cron->next(at, timers::host_zone())) {
    auto it = std::find_if(fires.begin(), fires.end(), [&](const Fire& f) { return f.timer == "minute" && f.due == at; });
    if (it == fires.end()) {
      write("minute", "", at, std::nullopt, "missing");
    } else {
      write("minute", "", at, it->at, minute_fires[at] > 1 ? "duplicate" : verdict_of(it->at - at));
    }
  }
  for (const auto& [due, n] : minute_fires) {
    if (due < wall_start || due > wall_end) write("minute", "", due, std::nullopt, "unexpected");
  }

  // remind: each one-shot row once, on its second.
  for (int64_t at : reminders) {
    std::vector<const Fire*> got;
    for (const auto& f : fires) {
      if (f.timer == "remind" && f.due == at) got.push_back(&f);
    }
    if (got.empty()) {
      write("remind", "", at, std::nullopt, at < wall_end - 3000 ? "missing" : "ok");
    } else {
      write("remind", got[0]->row, at, got[0]->at, got.size() > 1 ? "duplicate" : verdict_of(got[0]->at - at));
    }
  }

  // The wall clock against the monotonic clock: a step (NTP, a VM pause) shows here.
  int64_t skew = run_ms - mono_ms;
  bool clocks_agree = std::abs(skew) <= kClockSkewMs;
  if (!clocks_agree) ++failures;
  bj::object summary{{"summary", true},
                     {"pass", failures == 0},
                     {"failures", failures},
                     {"fires", int64_t(fires.size())},
                     {"beats", beats},
                     {"minutes", minutes},
                     {"started", base::format_utc_ms(wall_start)},
                     {"ended", base::format_utc_ms(wall_end)},
                     {"wall_minus_monotonic_ms", skew},
                     {"late_after_ms", kLateMs}};
  log << json::dump(summary) << "\n";
  log.close();
  std::cout << json::dump(summary) << "\n";
  fs::remove_all(root, ec);
  return failures == 0 ? 0 : 1;
}
