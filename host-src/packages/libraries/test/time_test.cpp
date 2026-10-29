#include <gtest/gtest.h>

#include "goober/calendar/zones.h"
#include "goober/libraries/libraries.h"

using namespace goober;
namespace bj = boost::json;

namespace {

bj::object call(const std::string& fn, const std::string& in, int64_t now = 1793286000000) {
  libraries::Context ctx{now};
  std::string out = libraries::call(*libraries::find("time", 1), fn, in, ctx);
  std::string err;
  auto v = json::parse(out, &err);
  EXPECT_TRUE(v && v->is_object()) << out;
  return v->as_object();
}

int64_t ms_of(calendar::Civil c) { return calendar::civil_seconds(c) * 1000; }

}  // namespace

TEST(Time, NowIsTheRequestsClock) {
  auto n = call("now", R"({})", 1793286000000);
  EXPECT_EQ(n["ms"].as_int64(), 1793286000000);
  EXPECT_EQ(n["utc"], "2026-10-29T15:00:00Z");
  auto local = call("now", R"({"zone":"America/Chicago"})", 1793286000000);
  EXPECT_EQ(local["local"], "2026-10-29T10:00:00");
  EXPECT_EQ(local["offset"], "-05:00");
  EXPECT_FALSE(local["tzdata"].as_string().empty());
}

TEST(Time, ParseFormatAndWallClock) {
  // After DST ends (Nov 1, 2026), Chicago is UTC-6.
  auto p = call("parse", R"({"text":"2026-11-05T18:00","zone":"America/Chicago"})");
  EXPECT_EQ(p["ms"].as_int64(), ms_of({2026, 11, 6, 0, 0, 0}));
  EXPECT_EQ(p["utc"], "2026-11-06T00:00:00Z");
  EXPECT_EQ(call("parse", R"({"text":"2026-11-05T18:00:00-06:00"})")["ms"].as_int64(), ms_of({2026, 11, 6, 0, 0, 0}));
  EXPECT_EQ(call("parse", R"({"text":"2026-11-06T00:00:00.250Z"})")["ms"].as_int64(), ms_of({2026, 11, 6, 0, 0, 0}) + 250);
  EXPECT_EQ(call("parse", R"({"text":"2026-11-05T18:00"})")["code"], "bad_input");  // no zone

  auto f = call("format", R"({"ms":1793923200000,"zone":"America/Chicago"})");
  EXPECT_EQ(f["local"], "2026-11-05T18:00:00");
  EXPECT_EQ(f["date"], "2026-11-05");
  EXPECT_EQ(f["time"], "18:00:00");
  EXPECT_EQ(f["offset"], "-06:00");
  EXPECT_EQ(f["abbreviation"], "CST");
  EXPECT_EQ(f["dst"], false);
  EXPECT_EQ(f["weekday"], "thursday");
  EXPECT_EQ(f["iso_week"], "2026-W45");
  EXPECT_EQ(f["day_of_year"].as_int64(), 309);

  auto gap = call("to_utc", R"({"local":"2026-03-08T02:30:00","zone":"America/Chicago"})");
  EXPECT_EQ(gap["adjusted"], "gap");
  EXPECT_EQ(gap["ms"].as_int64(), ms_of({2026, 3, 8, 8, 30, 0}));  // 03:30 CDT
  auto overlap = call("to_utc", R"({"local":{"date":"2026-11-01","time":"01:30"},"zone":"America/Chicago"})");
  EXPECT_EQ(overlap["adjusted"], "overlap");
  EXPECT_EQ(overlap["ms"].as_int64(), ms_of({2026, 11, 1, 6, 30, 0}));  // the earlier, CDT
  EXPECT_EQ(call("format", R"({"ms":0,"zone":"Mars/Olympus"})")["code"], "bad_input");
}

TEST(Time, CalendarArithmeticKeepsTheWallClock) {
  // A day across spring forward is 23 hours; the wall clock stays at 9:00.
  auto next = call("add", R"({"local":"2026-03-07T09:00:00","zone":"America/Chicago","days":1})");
  EXPECT_EQ(next["local"], "2026-03-08T09:00:00");
  EXPECT_EQ(next["ms"].as_int64() - ms_of({2026, 3, 7, 15, 0, 0}), 23 * 3600000LL);
  // Jan 31 plus a month is the end of February; exact hours after the calendar part.
  EXPECT_EQ(call("add", R"({"local":"2026-01-31T12:00:00","zone":"UTC","months":1})")["local"], "2026-02-28T12:00:00");
  EXPECT_EQ(call("add", R"({"local":"2028-01-31T12:00:00","zone":"UTC","months":1,"hours":13})")["local"], "2028-03-01T01:00:00");
  auto d = call("diff", R"({"from":"2026-03-07T15:00:00Z","to":"2026-03-08T14:00:00Z","zone":"America/Chicago"})");
  EXPECT_EQ(d["ms"].as_int64(), 23 * 3600000LL);
  EXPECT_EQ(d["days"].as_int64(), 1);
  auto week = call("start_of", R"({"ms":1793923200000,"zone":"America/Chicago","unit":"week"})");
  EXPECT_EQ(week["local"], "2026-11-02T00:00:00");
  EXPECT_EQ(call("start_of", R"({"ms":1793923200000,"zone":"America/Chicago","unit":"week","week_starts":"sunday"})")["local"],
            "2026-11-01T00:00:00");
  EXPECT_EQ(call("start_of", R"({"ms":1793923200000,"zone":"America/Chicago","unit":"month"})")["local"], "2026-11-01T00:00:00");
}

TEST(Time, ZonesAndRecurrence) {
  auto z = call("zone", R"({"zone":"America/Chicago","ms":"2026-07-01T12:00:00Z"})");
  EXPECT_EQ(z["abbreviation"], "CDT");
  EXPECT_EQ(z["offset"], "-05:00");
  EXPECT_EQ(z["next_change"].at("utc"), "2026-11-01T07:00:00Z");
  auto names = call("zones", R"({"prefix":"America/Chi"})");
  bool chicago = false;
  for (const auto& n : names["zones"].as_array()) chicago |= n == "America/Chicago";
  EXPECT_TRUE(chicago);

  auto r = call("recur", R"({"rrule":"FREQ=WEEKLY;BYDAY=TH;COUNT=3",
      "start":{"local":"2026-10-29T19:00:00","zone":"America/Chicago"}})");
  const auto& inst = r["instances"].as_array();
  ASSERT_EQ(inst.size(), 3u);
  // Across the DST change, still 19:00 on the wall: 00:00Z, then 01:00Z.
  EXPECT_EQ(inst[0].at("local"), "2026-10-29T19:00:00");
  EXPECT_EQ(inst[0].at("utc"), "2026-10-30T00:00:00Z");
  EXPECT_EQ(inst[1].at("local"), "2026-11-05T19:00:00");
  EXPECT_EQ(inst[1].at("utc"), "2026-11-06T01:00:00Z");
  EXPECT_EQ(r["truncated"], false);
  auto win = call("recur", R"({"rrule":"FREQ=DAILY","start":{"local":"2026-01-01T08:00:00","zone":"UTC"},
      "from":"2026-01-10T00:00:00Z","to":"2026-01-13T00:00:00Z","exdates":["2026-01-11T08:00:00Z"]})");
  ASSERT_EQ(win["instances"].as_array().size(), 2u);  // the 10th and 12th
  EXPECT_EQ(win["instances"].at(1).at("local"), "2026-01-12T08:00:00");
  auto capped = call("recur", R"({"rrule":"FREQ=DAILY","start":{"local":"2026-01-01T08:00:00","zone":"UTC"},"limit":5})");
  EXPECT_EQ(capped["instances"].as_array().size(), 5u);
  EXPECT_EQ(capped["truncated"], true);
  EXPECT_EQ(call("recur", R"({"rrule":"FREQ=SOMETIMES","start":{"ms":0,"zone":"UTC"}})")["code"], "bad_input");
}

TEST(Libraries, RegistryRefsAndErrors) {
  EXPECT_TRUE(libraries::find("time", 1));
  EXPECT_TRUE(libraries::find("ics", 1));
  EXPECT_FALSE(libraries::find("time", 2));
  EXPECT_EQ(libraries::parse_ref("time@1"), (std::pair<std::string, int>{"time", 1}));
  EXPECT_FALSE(libraries::parse_ref("time"));
  EXPECT_FALSE(libraries::parse_ref("Time@1"));
  EXPECT_FALSE(libraries::parse_ref("time@0"));
  libraries::Context ctx{0};
  auto unknown = libraries::call(*libraries::find("time", 1), "teleport", "{}", ctx);
  EXPECT_NE(unknown.find("\"code\":\"unknown_function\""), std::string::npos);
  EXPECT_NE(libraries::call(*libraries::find("time", 1), "now", "[1]", ctx).find("bad_input"), std::string::npos);
  auto doc = libraries::describe(*libraries::find("ics", 1));
  EXPECT_EQ(doc["library"], "ics@1");
  EXPECT_GE(doc["functions"].as_array().size(), 8u);
}
