// Recurrence against RFC 5545 § 3.8.5.3's own examples (DTSTART in America/New_York, 1997),
// and zones at their DST edges.

#include <gtest/gtest.h>

#include "goober/calendar/ics.h"
#include "goober/calendar/rrule.h"
#include "goober/calendar/zones.h"

using namespace goober::calendar;

namespace {

auto ny() { return iana("America/New_York"); }

// The instances' dates, as "YYYY-MM-DD" (times are 9:00 local unless the test says).
std::vector<std::string> dates(const std::string& dtstart, const std::string& rule, size_t max = 100,
                               const std::string& horizon = "2010-01-01") {
  bool date_only = false;
  auto start = *parse_civil_text(dtstart, &date_only);
  auto h = *parse_civil_text(horizon, &date_only);
  std::string err;
  auto r = parse_rule(rule, &err);
  EXPECT_TRUE(r) << rule << ": " << err;
  if (!r) return {};
  auto zone = ny();
  auto e = expand(start, *r, h, max, [&](const Civil& c) { return zone->to_utc(c, nullptr); });
  std::vector<std::string> out;
  for (const auto& c : e.instances) out.push_back(civil_text(c, true));
  return out;
}

using V = std::vector<std::string>;

}  // namespace

TEST(Rrule, DailyWeeklyAndIntervals) {
  EXPECT_EQ(dates("1997-09-02T09:00:00", "FREQ=DAILY;COUNT=10"),
            (V{"1997-09-02", "1997-09-03", "1997-09-04", "1997-09-05", "1997-09-06", "1997-09-07", "1997-09-08",
               "1997-09-09", "1997-09-10", "1997-09-11"}));
  EXPECT_EQ(dates("1997-09-02T09:00:00", "FREQ=DAILY;INTERVAL=10;COUNT=5"),
            (V{"1997-09-02", "1997-09-12", "1997-09-22", "1997-10-02", "1997-10-12"}));
  EXPECT_EQ(dates("1997-09-02T09:00:00", "FREQ=WEEKLY;COUNT=10"),
            (V{"1997-09-02", "1997-09-09", "1997-09-16", "1997-09-23", "1997-09-30", "1997-10-07", "1997-10-14",
               "1997-10-21", "1997-10-28", "1997-11-04"}));
  EXPECT_EQ(dates("1997-09-02T09:00:00", "FREQ=WEEKLY;COUNT=10;WKST=SU;BYDAY=TU,TH"),
            (V{"1997-09-02", "1997-09-04", "1997-09-09", "1997-09-11", "1997-09-16", "1997-09-18", "1997-09-23",
               "1997-09-25", "1997-09-30", "1997-10-02"}));
  // Every other week on Monday, Wednesday, and Friday until December 24, 1997.
  EXPECT_EQ(dates("1997-09-02T09:00:00", "FREQ=WEEKLY;INTERVAL=2;UNTIL=19971224T000000Z;WKST=SU;BYDAY=MO,WE,FR"),
            (V{"1997-09-02", "1997-09-03", "1997-09-05", "1997-09-15", "1997-09-17", "1997-09-19", "1997-09-29",
               "1997-10-01", "1997-10-03", "1997-10-13", "1997-10-15", "1997-10-17", "1997-10-27", "1997-10-29",
               "1997-10-31", "1997-11-10", "1997-11-12", "1997-11-14", "1997-11-24", "1997-11-26", "1997-11-28",
               "1997-12-08", "1997-12-10", "1997-12-12", "1997-12-22"}));
}

TEST(Rrule, MonthlyOrdinalsNegativeDaysAndSetPositions) {
  EXPECT_EQ(dates("1997-09-05T09:00:00", "FREQ=MONTHLY;COUNT=10;BYDAY=1FR"),
            (V{"1997-09-05", "1997-10-03", "1997-11-07", "1997-12-05", "1998-01-02", "1998-02-06", "1998-03-06",
               "1998-04-03", "1998-05-01", "1998-06-05"}));
  EXPECT_EQ(dates("1997-09-22T09:00:00", "FREQ=MONTHLY;COUNT=6;BYDAY=-2MO"),
            (V{"1997-09-22", "1997-10-20", "1997-11-17", "1997-12-22", "1998-01-19", "1998-02-16"}));
  EXPECT_EQ(dates("1997-09-28T09:00:00", "FREQ=MONTHLY;BYMONTHDAY=-3", 6),
            (V{"1997-09-28", "1997-10-29", "1997-11-28", "1997-12-29", "1998-01-29", "1998-02-26"}));
  EXPECT_EQ(dates("1997-09-04T09:00:00", "FREQ=MONTHLY;COUNT=3;BYDAY=TU,WE,TH;BYSETPOS=3"),
            (V{"1997-09-04", "1997-10-07", "1997-11-06"}));
  // The last weekday of the month.
  EXPECT_EQ(dates("1997-09-30T09:00:00", "FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1", 7),
            (V{"1997-09-30", "1997-10-31", "1997-11-28", "1997-12-31", "1998-01-30", "1998-02-27", "1998-03-31"}));
  // Friday the 13th (RFC 5545 excludes DTSTART with EXDATE; here it's dropped by hand).
  auto f13 = dates("1997-09-02T09:00:00", "FREQ=MONTHLY;BYDAY=FR;BYMONTHDAY=13", 6);
  f13.erase(f13.begin());
  EXPECT_EQ(f13, (V{"1998-02-13", "1998-03-13", "1998-11-13", "1999-08-13", "2000-10-13"}));
}

TEST(Rrule, YearlyByMonthYearDayWeekNumberAndOrdinal) {
  EXPECT_EQ(dates("1997-06-10T09:00:00", "FREQ=YEARLY;COUNT=10;BYMONTH=6,7"),
            (V{"1997-06-10", "1997-07-10", "1998-06-10", "1998-07-10", "1999-06-10", "1999-07-10", "2000-06-10",
               "2000-07-10", "2001-06-10", "2001-07-10"}));
  EXPECT_EQ(dates("1997-01-01T09:00:00", "FREQ=YEARLY;INTERVAL=3;COUNT=10;BYYEARDAY=1,100,200"),
            (V{"1997-01-01", "1997-04-10", "1997-07-19", "2000-01-01", "2000-04-09", "2000-07-18", "2003-01-01",
               "2003-04-10", "2003-07-19", "2006-01-01"}));
  EXPECT_EQ(dates("1997-03-13T09:00:00", "FREQ=YEARLY;BYMONTH=3;BYDAY=TH", 11),
            (V{"1997-03-13", "1997-03-20", "1997-03-27", "1998-03-05", "1998-03-12", "1998-03-19", "1998-03-26",
               "1999-03-04", "1999-03-11", "1999-03-18", "1999-03-25"}));
  EXPECT_EQ(dates("1997-05-12T09:00:00", "FREQ=YEARLY;BYWEEKNO=20;BYDAY=MO", 3),
            (V{"1997-05-12", "1998-05-11", "1999-05-17"}));
  EXPECT_EQ(dates("1997-05-19T09:00:00", "FREQ=YEARLY;BYDAY=20MO", 3), (V{"1997-05-19", "1998-05-18", "1999-05-17"}));
  // U.S. Presidential election day: the Tuesday after the first Monday of November, every four years.
  EXPECT_EQ(dates("1996-11-05T09:00:00", "FREQ=YEARLY;INTERVAL=4;BYMONTH=11;BYDAY=TU;BYMONTHDAY=2,3,4,5,6,7,8", 3),
            (V{"1996-11-05", "2000-11-07", "2004-11-02"}));
  // Thanksgiving, checked against its definition (the fourth Thursday of November).
  auto tg = dates("2026-11-26T18:00:00", "FREQ=YEARLY;BYMONTH=11;BYDAY=4TH", 10, "2050-01-01");
  ASSERT_EQ(tg.size(), 10u);
  for (const auto& d : tg) {
    int y = std::stoi(d.substr(0, 4)), day = std::stoi(d.substr(8, 2));
    EXPECT_EQ(weekday(y, 11, day), 3) << d;
    EXPECT_GE(day, 22);
    EXPECT_LE(day, 28);
  }
}

TEST(Rrule, TimesStayOnTheWallClockAcrossDst) {
  // Weekly at 9:00 in New York: 13:00Z in summer, 14:00Z after the clocks go back.
  bool d = false;
  auto start = *parse_civil_text("1997-10-21T09:00:00", &d);
  std::string err;
  auto r = *parse_rule("FREQ=WEEKLY;COUNT=3", &err);
  auto zone = ny();
  auto e = expand(start, r, Civil{2000, 1, 1}, 10, [&](const Civil& c) { return zone->to_utc(c, nullptr); });
  ASSERT_EQ(e.instances.size(), 3u);
  EXPECT_EQ(zone->to_utc(e.instances[0], nullptr), civil_seconds(Civil{1997, 10, 21, 13, 0, 0}) * 1000);
  EXPECT_EQ(zone->to_utc(e.instances[1], nullptr), civil_seconds(Civil{1997, 10, 28, 14, 0, 0}) * 1000);
  // Limits: a rule that never matches stops, truncated, instead of spinning.
  auto never = *parse_rule("FREQ=YEARLY;BYMONTH=2;BYMONTHDAY=30", &err);
  auto none = expand(Civil{2026, 1, 1}, never, Civil{2100, 1, 1}, 10, [](const Civil& c) { return civil_seconds(c) * 1000; });
  EXPECT_EQ(none.instances.size(), 1u);  // dtstart alone
  auto many = expand(Civil{2026, 1, 1}, *parse_rule("FREQ=DAILY", &err), Civil{2100, 1, 1}, 50,
                     [](const Civil& c) { return civil_seconds(c) * 1000; });
  EXPECT_EQ(many.instances.size(), 50u);
  EXPECT_TRUE(many.truncated);
}

TEST(Rrule, ParsingRefusesWhatItCantMeanAndWritesItBack) {
  std::string err;
  for (const char* bad : {"BYDAY=MO", "FREQ=FORTNIGHTLY", "FREQ=DAILY;COUNT=0", "FREQ=DAILY;COUNT=3;UNTIL=20261231",
                          "FREQ=MONTHLY;BYWEEKNO=3", "FREQ=WEEKLY;BYDAY=XX", "FREQ=DAILY;BYHOUR=24", "FREQ=DAILY;NOPE=1"}) {
    EXPECT_FALSE(parse_rule(bad, &err)) << bad;
  }
  auto r = parse_rule("FREQ=MONTHLY;INTERVAL=2;BYDAY=-1FR,2MO;UNTIL=20271231T235959Z;WKST=SU", &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(format_rule(*r), "FREQ=MONTHLY;INTERVAL=2;UNTIL=20271231T235959Z;BYDAY=-1FR,2MO;WKST=SU");
}

TEST(Zones, GapsOverlapsAndTheCalendarsOwnRules) {
  auto chicago = iana("America/Chicago");
  ASSERT_TRUE(chicago);
  int adjusted = 0;
  // 2026-03-08 02:30 doesn't exist in Chicago: it lands at 03:30 CDT (08:30Z).
  EXPECT_EQ(chicago->to_utc(Civil{2026, 3, 8, 2, 30, 0}, &adjusted), civil_seconds(Civil{2026, 3, 8, 8, 30, 0}) * 1000);
  EXPECT_EQ(adjusted, 1);
  // 2026-11-01 01:30 happens twice: the earlier, CDT (06:30Z).
  EXPECT_EQ(chicago->to_utc(Civil{2026, 11, 1, 1, 30, 0}, &adjusted), civil_seconds(Civil{2026, 11, 1, 6, 30, 0}) * 1000);
  EXPECT_EQ(adjusted, 2);
  EXPECT_EQ(chicago->at(civil_seconds(Civil{2026, 7, 1}) * 1000).offset_s, -5 * 3600);
  EXPECT_TRUE(chicago->at(civil_seconds(Civil{2026, 7, 1}) * 1000).dst);
  EXPECT_EQ(chicago->at(civil_seconds(Civil{2026, 12, 1}) * 1000).abbreviation, "CST");
  EXPECT_FALSE(iana("Nowhere/Atlantis"));
  EXPECT_EQ(windows_to_iana("Central Standard Time"), "America/Chicago");
  EXPECT_FALSE(tzdata_version().empty());

  // Outlook's VTIMEZONE for Central time, evaluated from its rules, agrees with IANA.
  std::string ics =
      "BEGIN:VCALENDAR\r\nBEGIN:VTIMEZONE\r\nTZID:Central Standard Time\r\nBEGIN:STANDARD\r\nDTSTART:16010101T020000\r\n"
      "TZOFFSETFROM:-0500\r\nTZOFFSETTO:-0600\r\nRRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=1SU;BYMONTH=11\r\nEND:STANDARD\r\n"
      "BEGIN:DAYLIGHT\r\nDTSTART:16010101T020000\r\nTZOFFSETFROM:-0600\r\nTZOFFSETTO:-0500\r\n"
      "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=2SU;BYMONTH=3\r\nEND:DAYLIGHT\r\nEND:VTIMEZONE\r\nEND:VCALENDAR\r\n";
  std::string err;
  auto parsed = parse(ics, &err);
  ASSERT_TRUE(parsed) << err;
  auto zones = calendar_zones(parsed->components[0], nullptr);
  ASSERT_TRUE(zones.count("Central Standard Time"));
  auto rules = zones.at("Central Standard Time");
  for (const Civil& when : {Civil{2026, 1, 15, 12}, Civil{2026, 3, 8, 7}, Civil{2026, 3, 8, 9}, Civil{2026, 7, 4, 12},
                            Civil{2026, 11, 1, 6}, Civil{2026, 11, 1, 8}, Civil{2027, 3, 14, 9}}) {
    int64_t ms = civil_seconds(when) * 1000;
    EXPECT_EQ(rules->at(ms).offset_s, chicago->at(ms).offset_s) << civil_text(when);
  }
  EXPECT_EQ(rules->to_utc(Civil{2026, 7, 4, 18, 0, 0}, nullptr), chicago->to_utc(Civil{2026, 7, 4, 18, 0, 0}, nullptr));
}
