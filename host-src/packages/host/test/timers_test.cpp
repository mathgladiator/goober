// Timers (045.Time.md; design/machine/014.Time.md), run against an injected clock.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace std::chrono;

namespace {

int64_t utc(int y, int mo, int d, int h, int mi, int s = 0) {
  return duration_cast<milliseconds>((sys_days(year(y) / mo / d) + hours(h) + minutes(mi) + seconds(s)).time_since_epoch())
      .count();
}

// Each fire writes one row naming its timer. None responds: a fire may just return.
const char* kClock = R"wat((module
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "w")
  (data (i32.const 16) "INSERT INTO ticks (what) VALUES (?)")
  (data (i32.const 64) "[\22beat\22]")
  (data (i32.const 80) "[\22morning\22]")
  (data (i32.const 96) "[\22remind\22]")
  (func $tick (param $p i32) (param $n i32)
    (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 35) (local.get $p) (local.get $n))))
  (func (export "goober_route_beat") (result i32) (call $tick (i32.const 64) (i32.const 8)) (i32.const 0))
  (func (export "goober_route_morning") (result i32) (call $tick (i32.const 80) (i32.const 11)) (i32.const 0))
  (func (export "goober_route_remind") (result i32) (call $tick (i32.const 96) (i32.const 10)) (i32.const 0))
))wat";

const char* kSchema = R"({"schema":"clock1","tables":{
  "ticks":{"columns":{"what":{"type":"string","max-length":40}}},
  "people":{"columns":{"name":{"type":"string"},"timezone":{"type":"string"},"on":{"type":"boolean","default":true}}},
  "reminders":{"columns":{"remind_at":{"type":"int64"},"on":{"type":"boolean","default":true}}}}})";

const char* kManifest = R"({"box":"clock","domains":["*"],
  "data":{"sqlite3":{
    "w":{"schema":"clock1","database":"clock","pattern":"sql","tables":{"ticks":{"read":true,"insert":true}}},
    "people_t":{"schema":"clock1","database":"clock","pattern":"by_timer","table":"people",
                "timezone-field":"timezone","active-field":"on"},
    "remind_t":{"schema":"clock1","database":"clock","pattern":"by_timer","table":"reminders","active-field":"on"}}},
  "routes":{"timer":{
    "beat":{"name":"beat","every-ms":60000},
    "morning":{"name":"morning","source":"people_t","cron":"0 6 * * *"},
    "remind":{"name":"remind","source":"remind_t","at-field":"remind_at"}}}})";

}  // namespace

class TimersTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "timers";
    fs::remove_all(root);
    start();
    std::string err;
    ASSERT_TRUE(host->schemas().put(kSchema, &err)) << err;
    ASSERT_TRUE(host->boxes().upload(kManifest, kClock, &err)) << err;
    auto schema = host->schemas().get("clock1");
    ASSERT_TRUE(host->databases().open("playground", "clock", &*schema, &err)) << err;
    exec("INSERT INTO people (name, timezone) VALUES ('Ada', 'America/Chicago'), ('Kenji', 'Asia/Tokyo'), "
         "('Nowhere', 'Mars/Olympus')");
    exec("INSERT INTO reminders (remind_at) VALUES (" + std::to_string(utc(2026, 10, 27, 10, 30)) + ")");
  }
  void start() {
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    host->set_timers_inline(true);
  }
  void exec(const std::string& sql) {
    std::string err;
    ASSERT_TRUE(host->databases().operate("playground", "clock", sql, nullptr, 100, true, &err)) << err;
  }
  int64_t count(const char* what) {
    std::string err;
    auto r = host->databases().operate("playground", "clock", "SELECT count(*) FROM ticks WHERE what = ?",
                                       bj::array{what}, 10, false, &err);
    return r ? r->at("rows").at(0).at(0).as_int64() : -1;
  }
  fs::path root;
  std::unique_ptr<host::Host> host;
};

TEST_F(TimersTest, ADayOfSchedulesInMilliseconds) {
  host->timer_tick(utc(2026, 10, 27, 10, 0));  // plans everything; nothing is due yet
  EXPECT_EQ(count("beat"), 0);
  host->timer_tick(utc(2026, 10, 27, 10, 1, 1));
  EXPECT_EQ(count("beat"), 1);

  // The reminder fires once, and the host turns its row off with the fire's writes.
  host->timer_tick(utc(2026, 10, 27, 10, 30, 1));
  EXPECT_EQ(count("remind"), 1);
  host->timer_tick(utc(2026, 10, 27, 10, 30, 20));
  EXPECT_EQ(count("remind"), 1);
  std::string err;
  auto on = host->databases().operate("playground", "clock", "SELECT \"on\" FROM reminders", nullptr, 10, false, &err);
  EXPECT_EQ(on->at("rows").at(0).at(0), 0);

  // 6am in each person's own zone: Chicago at 11:00 UTC, Tokyo at 21:00 UTC; Mars never.
  host->timer_tick(utc(2026, 10, 27, 10, 59, 30));
  EXPECT_EQ(count("morning"), 0);
  host->timer_tick(utc(2026, 10, 27, 11, 0, 1));
  EXPECT_EQ(count("morning"), 1);
  host->timer_tick(utc(2026, 10, 27, 21, 0, 1));
  EXPECT_EQ(count("morning"), 2);
  EXPECT_EQ(count("beat"), 4);  // once a minute when ticked, never piling up

  // Down overnight: on restart, Chicago's missed 6am runs once; Tokyo's isn't missed yet.
  host.reset();
  start();
  host->timer_tick(utc(2026, 10, 28, 12, 0));
  EXPECT_EQ(count("morning"), 3);
  host->timer_tick(utc(2026, 10, 28, 12, 0, 30));
  EXPECT_EQ(count("morning"), 3);
}

TEST_F(TimersTest, OperatorsSeeThePlanAndFireNow) {
  host->timer_tick(utc(2026, 10, 27, 10, 0));
  auto plan = host->timers_due();
  bool beat = false, chicago = false;
  for (const auto& p : plan) {
    beat |= p.at("timer") == "beat";
    chicago |= p.at("timer") == "morning" && p.at("next_due") == "2026-10-27T11:00:00.000Z";
  }
  EXPECT_TRUE(beat);
  EXPECT_TRUE(chicago);
  std::string err;
  auto fired = host->run_timer_now("morning", "playground", &err);
  ASSERT_TRUE(fired) << err;
  EXPECT_EQ(*fired, 3);  // every active row, now
  EXPECT_EQ(count("morning"), 3);
  EXPECT_FALSE(host->run_timer_now("morning", "elsewhere", &err));
  EXPECT_FALSE(host->run_timer_now("nope", "playground", &err));
}

// Quality review 4: two boxes with the same strategy name keep their own rows.
TEST_F(TimersTest, RowsBelongToTheirBox) {
  std::string err;
  ASSERT_TRUE(host->schemas().put(R"({"schema":"notes1","tables":{
    "ticks":{"columns":{"what":{"type":"string","max-length":40}}},
    "reminders":{"columns":{"remind_at":{"type":"int64"},
                            "on":{"type":"boolean","default":true}}}}})", &err)) << err;
  const char* wat = R"wat((module
    (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
    (memory (export "memory") 1)
    (data (i32.const 0) "w")
    (data (i32.const 16) "INSERT INTO ticks (what) VALUES (?)")
    (data (i32.const 64) "[\22nudge\22]")
    (func (export "goober_route_nudge") (result i32)
      (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 35) (i32.const 64) (i32.const 9)))
      (i32.const 0))))wat";
  // Its strategy is named remind_t, like clock's, over another database.
  ASSERT_TRUE(host->boxes().upload(R"({"box":"notes","domains":["*"],
    "data":{"sqlite3":{
      "w":{"schema":"notes1","database":"notes","pattern":"sql","tables":{"ticks":{"read":true,"insert":true}}},
      "remind_t":{"schema":"notes1","database":"notes","pattern":"by_timer","table":"reminders","active-field":"on"}}},
    "routes":{"timer":{"nudge":{"name":"nudge","source":"remind_t","at-field":"remind_at"}}}})", wat, &err)) << err;
  auto schema = host->schemas().get("notes1");
  ASSERT_TRUE(host->databases().open("playground", "notes", &*schema, &err)) << err;
  ASSERT_TRUE(host->databases().operate("playground", "notes",
      "INSERT INTO reminders (remind_at) VALUES (" + std::to_string(utc(2026, 10, 27, 9, 0)) + ")",
      nullptr, 10, true, &err)) << err;
  auto notes_count = [&] {
    auto r = host->databases().operate("playground", "notes", "SELECT count(*) FROM ticks WHERE what = 'nudge'", nullptr, 10,
                                       false, &err);
    return r ? r->at("rows").at(0).at(0).as_int64() : -1;
  };
  host->timer_tick(utc(2026, 10, 27, 10, 30, 1));
  EXPECT_EQ(count("remind"), 1);  // clock's reminder, from clock's own rows
  EXPECT_EQ(notes_count(), 1);    // notes' reminder, from notes' own rows
  host->timer_tick(utc(2026, 10, 27, 10, 30, 2));
  host->timer_tick(utc(2026, 10, 27, 10, 30, 20));
  EXPECT_EQ(notes_count(), 1);  // turned off
  EXPECT_EQ(count("remind"), 1);
  auto on = host->databases().operate("playground", "notes", "SELECT \"on\" FROM reminders", nullptr, 10, false, &err);
  EXPECT_EQ(on->at("rows").at(0).at(0), 0);
}

// Firing an at-field timer now turns its rows off too, so the schedule doesn't fire them again.
TEST_F(TimersTest, FiringAtFieldTimersNowTurnsRowsOff) {
  std::string err;
  auto fired = host->run_timer_now("remind", "playground", &err);
  ASSERT_TRUE(fired) << err;
  EXPECT_EQ(*fired, 1);
  host->timer_tick(utc(2026, 10, 27, 10, 30, 1));
  EXPECT_EQ(count("remind"), 1);
}

// Code review 4: a new version's schedule takes effect at once, and removed rows leave no
// ghosts in the plan.
TEST_F(TimersTest, PlansFollowTheScheduleAndTheRows) {
  host->timer_tick(utc(2026, 10, 27, 10, 0));
  auto due_for = [&](const std::string& timer) {
    std::vector<std::string> out;
    for (const auto& p : host->timers_due()) {
      if (p.at("timer").as_string() == timer) out.push_back(std::string(p.at("next_due").as_string()));
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  EXPECT_EQ(due_for("morning").front(), "2026-10-27T11:00:00.000Z");  // 6am in Chicago
  std::string manifest = kManifest;
  manifest.replace(manifest.find("0 6 * * *"), 9, "0 7 * * *");
  std::string err;
  ASSERT_TRUE(host->boxes().upload(manifest, kClock, &err)) << err;
  host->timer_tick(utc(2026, 10, 27, 10, 0, 5));
  EXPECT_EQ(due_for("morning").front(), "2026-10-27T12:00:00.000Z");  // now 7am
  exec("DELETE FROM people WHERE name = 'Kenji'");
  host->timer_tick(utc(2026, 10, 27, 10, 5));  // past the rows' refresh
  EXPECT_EQ(due_for("morning").size(), 1u);    // Chicago only; Mars never had a plan
}
