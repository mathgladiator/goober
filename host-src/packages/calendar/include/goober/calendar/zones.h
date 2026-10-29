#pragma once

// Civil time and time zones (design/machine/831.Libraries.md § 4): wall-clock dates and times,
// IANA zones from the server's tzdata (through C++20 chrono, as timers use), zones a calendar
// defines itself (VTIMEZONE), and Windows zone names mapped to IANA.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goober::calendar {

// A wall-clock time: no zone of its own.
struct Civil {
  int64_t y = 1970;
  int mo = 1, d = 1, h = 0, mi = 0, s = 0;
  bool operator==(const Civil&) const = default;
  auto operator<=>(const Civil&) const = default;
};

int64_t days_from_civil(int64_t y, unsigned m, unsigned d);
void civil_from_days(int64_t days, int64_t* y, unsigned* m, unsigned* d);
bool leap_year(int64_t y);
int days_in_month(int64_t y, int m);
int day_of_year(const Civil& c);
// 0 = Monday ... 6 = Sunday.
int weekday(int64_t y, int m, int d);
bool valid(const Civil& c);
// The civil time as if it were UTC, in seconds (wall-clock arithmetic).
int64_t civil_seconds(const Civil& c);
Civil civil_from_seconds(int64_t s);
// "2026-11-05T18:00:00" / "2026-11-05".
std::string civil_text(const Civil& c, bool date_only = false);
std::optional<Civil> parse_civil_text(std::string_view text, bool* date_only);

class Zone {
 public:
  virtual ~Zone() = default;
  struct Info {
    int32_t offset_s = 0;
    bool dst = false;
    std::string abbreviation;
  };
  virtual std::string name() const = 0;
  virtual Info at(int64_t utc_ms) const = 0;
  // A wall-clock time to an instant. In a gap (spring forward), the time after it (adjusted 1);
  // in an overlap (fall back), the earlier instant (adjusted 2).
  virtual int64_t to_utc(const Civil& local, int* adjusted) const;
  // The next instant after utc_ms when the offset changes, if the zone knows it.
  virtual std::optional<int64_t> next_change(int64_t utc_ms) const;
  Civil to_local(int64_t utc_ms) const;
};

std::shared_ptr<const Zone> utc();
// An IANA zone (America/Chicago), or nullptr when tzdata doesn't have it.
std::shared_ptr<const Zone> iana(std::string_view name);
// A fixed offset, like +05:30 (the zone name is the offset).
std::shared_ptr<const Zone> fixed(int32_t offset_s);
// The server's tzdata version, like "2025b".
std::string tzdata_version();
std::vector<std::string> zone_names();
// Windows' name for a zone (Central Standard Time) as IANA's (America/Chicago), from CLDR's
// mapping for the common zones.
std::optional<std::string> windows_to_iana(std::string_view windows_name);

// One observance of a VTIMEZONE (RFC 5545 § 3.6.5): from its onset (a wall-clock time in the
// offset before it), the zone is offset_to; recurrences come from `rrule` and `rdates`.
struct Observance {
  bool daylight = false;
  Civil start;
  int32_t offset_from = 0, offset_to = 0;
  std::string rrule;  // as written, or ""
  std::vector<Civil> rdates;
  std::string name;
};
std::shared_ptr<const Zone> rules_zone(std::string tzid, std::vector<Observance> observances);

// "+0530", "-0600", "+05:30", "Z" as seconds east of UTC.
std::optional<int32_t> parse_offset(std::string_view text);
std::string offset_text(int32_t offset_s, bool colon = true);

}  // namespace goober::calendar
