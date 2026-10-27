#include <gtest/gtest.h>

#include "goober/timers/cron.h"

using namespace goober::timers;
using namespace std::chrono;

namespace {
int64_t utc(int y, int mo, int d, int h, int mi) {
  return duration_cast<milliseconds>((sys_days(year(y) / mo / d) + hours(h) + minutes(mi)).time_since_epoch()).count();
}
Cron cron(const char* text) {
  std::string err;
  auto c = Cron::parse(text, &err);
  EXPECT_TRUE(c) << err;
  return *c;
}
}  // namespace

TEST(Cron, ParsesAndRefuses) {
  std::string err;
  EXPECT_TRUE(Cron::parse("*/15 9-17 * * 1-5", &err));
  EXPECT_TRUE(Cron::parse("0 6 * * *", &err));
  EXPECT_TRUE(Cron::parse("5,35 0 1 1,7 0", &err));
  EXPECT_FALSE(Cron::parse("0 6 * *", &err));
  EXPECT_FALSE(Cron::parse("60 * * * *", &err));
  EXPECT_FALSE(Cron::parse("* 24 * * *", &err));
  EXPECT_FALSE(Cron::parse("* * 0 * *", &err));
  EXPECT_FALSE(Cron::parse("* * * * 8", &err));
  EXPECT_FALSE(Cron::parse("5-1 * * * *", &err));
  EXPECT_FALSE(Cron::parse("*/0 * * * *", &err));
  EXPECT_FALSE(Cron::parse("MON * * * *", &err));
}

TEST(Cron, SixAmInEachZone) {
  auto six = cron("0 6 * * *");
  auto chicago = find_zone("America/Chicago");
  auto tokyo = find_zone("Asia/Tokyo");
  ASSERT_TRUE(chicago && tokyo);
  // 2026-10-27 10:00 UTC is 5am in Chicago (CDT, UTC-5): next is 11:00 UTC the same day.
  EXPECT_EQ(*six.next(utc(2026, 10, 27, 10, 0), chicago), utc(2026, 10, 27, 11, 0));
  // In Tokyo (UTC+9) it is already 7pm, so next is 6am tomorrow, 21:00 UTC today.
  EXPECT_EQ(*six.next(utc(2026, 10, 27, 10, 0), tokyo), utc(2026, 10, 27, 21, 0));
  // Strictly after: asking at the fire time gives the next day's.
  EXPECT_EQ(*six.next(utc(2026, 10, 27, 11, 0), chicago), utc(2026, 10, 28, 11, 0));
  EXPECT_EQ(*six.previous(utc(2026, 10, 27, 11, 30), chicago), utc(2026, 10, 27, 11, 0));
  EXPECT_FALSE(find_zone("Mars/Olympus"));
}

TEST(Cron, DaylightSavingRules) {
  auto chicago = find_zone("America/Chicago");
  // Spring forward, 2026-03-08: 2:30am doesn't exist, so it fires at 3:00am CDT (08:00 UTC).
  auto half_two = cron("30 2 * * *");
  EXPECT_EQ(*half_two.next(utc(2026, 3, 8, 6, 0), chicago), utc(2026, 3, 8, 8, 0));
  // Fall back, 2026-11-01: 1:30am happens twice; it fires the first time only (06:30 UTC),
  // and the next is the following day.
  auto half_one = cron("30 1 * * *");
  int64_t first = *half_one.next(utc(2026, 11, 1, 5, 0), chicago);
  EXPECT_EQ(first, utc(2026, 11, 1, 6, 30));
  EXPECT_EQ(*half_one.next(first, chicago), utc(2026, 11, 2, 7, 30));
}

TEST(Cron, DayFieldsStepsAndNever) {
  auto utc_zone = find_zone("UTC");
  // Weekdays at 9:00 and 9:30: from Friday evening, the next is Monday.
  auto weekdays = cron("0,30 9 * * 1-5");
  EXPECT_EQ(*weekdays.next(utc(2026, 10, 30, 18, 0), utc_zone), utc(2026, 11, 2, 9, 0));
  // Both day fields restricted: the 1st or any Sunday.
  auto either = cron("0 0 1 * 0");
  EXPECT_EQ(*either.next(utc(2026, 10, 27, 0, 0), utc_zone), utc(2026, 11, 1, 0, 0));  // a Sunday and the 1st
  EXPECT_EQ(*either.next(utc(2026, 11, 1, 0, 0), utc_zone), utc(2026, 11, 8, 0, 0));
  EXPECT_EQ(*cron("*/20 * * * *").next(utc(2026, 1, 1, 0, 5), utc_zone), utc(2026, 1, 1, 0, 20));
  EXPECT_EQ(*cron("0 0 * * 7").next(utc(2026, 10, 27, 0, 0), utc_zone), utc(2026, 11, 1, 0, 0));  // 7 is Sunday
  EXPECT_FALSE(cron("0 0 31 2 *").next(utc(2026, 1, 1, 0, 0), utc_zone));
}

TEST(Cron, InstantsFromColumns) {
  EXPECT_EQ(*parse_instant_ms("2026-10-27T13:00:00Z"), utc(2026, 10, 27, 13, 0));
  EXPECT_EQ(*parse_instant_ms("2026-10-27T13:00:00.250Z"), utc(2026, 10, 27, 13, 0) + 250);
  EXPECT_EQ(*parse_instant_ms("1793000000000"), 1793000000000);
  EXPECT_FALSE(parse_instant_ms("2026-10-27 13:00"));
  EXPECT_FALSE(parse_instant_ms("2026-02-30T00:00:00Z"));
  EXPECT_FALSE(parse_instant_ms(""));
}
