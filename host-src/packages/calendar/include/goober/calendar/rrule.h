#pragma once

// Recurrence rules (RFC 5545 § 3.3.10, § 3.8.5): parsing, writing, and expansion in wall-clock
// time, so a 9:00 meeting stays at 9:00 across daylight saving (design/machine/831.Libraries.md).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goober/calendar/zones.h"

namespace goober::calendar {

enum class Freq { kSecondly, kMinutely, kHourly, kDaily, kWeekly, kMonthly, kYearly };

struct Rule {
  Freq freq = Freq::kDaily;
  int interval = 1;
  std::optional<int64_t> count;
  // UNTIL: a UTC instant (…Z), a wall-clock time, or a date (all inclusive).
  std::optional<int64_t> until_utc_ms;
  std::optional<Civil> until_local;
  bool until_date = false;
  std::vector<int> bysecond, byminute, byhour;
  std::vector<std::pair<int, int>> byday;  // (ordinal or 0, weekday 0 = MO .. 6 = SU)
  std::vector<int> bymonthday, byyearday, byweekno, bymonth, bysetpos;
  int wkst = 0;  // MO
};

std::optional<Rule> parse_rule(std::string_view text, std::string* error);
std::string format_rule(const Rule& rule);

struct Expansion {
  std::vector<Civil> instances;
  bool truncated = false;  // stopped at max, at the horizon, or at the iteration cap
};

// The rule's instances from dtstart (itself always the first), in order: those after
// `horizon` aren't made, and at most `max`. `to_utc` places a wall-clock time in its zone, for
// an UNTIL given in UTC.
Expansion expand(const Civil& dtstart, const Rule& rule, const Civil& horizon, size_t max,
                 const std::function<int64_t(const Civil&)>& to_utc);

}  // namespace goober::calendar
