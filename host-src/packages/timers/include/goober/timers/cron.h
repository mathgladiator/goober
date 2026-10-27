#pragma once

// Cron schedules in a time zone (045.Time.md): "minute hour day-of-month month day-of-week",
// with *, numbers, ranges a-b, lists a,b, and steps */n or a-b/n. Day of week is 0 to 7,
// where 0 and 7 are Sunday. When both day fields are restricted, either one matching is
// enough, as in classic cron. Times are computed in the zone's wall clock: a local time that
// doesn't exist (spring forward) fires at the next time that does, and one that happens twice
// (fall back) fires only the first time. See design/machine/014.Time.md.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace goober::timers {

class Cron {
 public:
  static std::optional<Cron> parse(std::string_view text, std::string* error);

  // The first fire strictly after `after_ms` (Unix milliseconds), in `zone`; nullopt when
  // the schedule never fires again within eight years (like "0 0 31 2 *").
  std::optional<int64_t> next(int64_t after_ms, const std::chrono::time_zone* zone) const;
  // The latest fire at or before `at_ms`, within a year back, for catching up after downtime.
  std::optional<int64_t> previous(int64_t at_ms, const std::chrono::time_zone* zone) const;

  const std::string& text() const { return text_; }

 private:
  uint64_t minutes_ = 0;  // bit m set when minute m matches
  uint32_t hours_ = 0;
  uint32_t days_ = 0;     // bit d for day of month 1..31
  uint16_t months_ = 0;   // bit m for 1..12
  uint8_t weekdays_ = 0;  // bit d for 0..6, Sunday = 0
  bool any_day_ = false;
  bool any_weekday_ = false;
  std::string text_;
  bool day_matches(std::chrono::year_month_day ymd) const;
};

// An instant from a timer column (045 at-field): Unix milliseconds as a number or a string of
// digits, or an RFC 3339 time in UTC like 2026-10-27T13:00:00Z.
std::optional<int64_t> parse_instant_ms(std::string_view text);

// A zone by IANA name, like America/Chicago, or nullptr.
const std::chrono::time_zone* find_zone(std::string_view name);
// The host's own zone (from /etc/localtime), or UTC.
const std::chrono::time_zone* host_zone();

}  // namespace goober::timers
