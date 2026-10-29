#pragma once

// iCalendar (RFC 5545): the content-line model, a parser that is lenient in what it reads
// and bounded in what it keeps, a serializer every client accepts, and typed values (text,
// dates and times with their zones, durations). design/machine/831.Libraries.md § 5.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/calendar/zones.h"

namespace goober::calendar {

struct Param {
  std::string name;  // uppercase
  std::vector<std::string> values;
};

struct Property {
  std::string name;  // uppercase
  std::vector<Param> params;
  std::string value;  // as written, unescaped later by its type
  const Param* param(std::string_view name) const;
  std::string param_value(std::string_view name) const;  // the first value, or ""
};

struct Component {
  std::string name;  // VCALENDAR, VEVENT, VTIMEZONE, STANDARD, DAYLIGHT, VALARM, ...
  std::vector<Property> props;
  std::vector<Component> children;
  const Property* prop(std::string_view name) const;
  std::vector<const Property*> all(std::string_view name) const;
  std::string text(std::string_view name) const;  // a TEXT property, unescaped, or ""
};

struct Parsed {
  std::vector<Component> components;  // usually one VCALENDAR
  std::vector<std::string> warnings;
};

inline constexpr size_t kMaxIcsBytes = 1 << 20;
// The input as components. Folded lines, LF or CRLF, and any case are read; past the limits
// (1 MiB, 10,000 properties a component, 8 deep) what's left is dropped with a warning.
std::optional<Parsed> parse(std::string_view text, std::string* error);
// CRLF, lines folded at 75 octets on UTF-8 boundaries, parameter values quoted when needed.
std::string serialize(const Component& c);

std::string unescape_text(std::string_view v);
std::string escape_text(std::string_view v);

// A DATE or DATE-TIME value, resolved.
struct When {
  bool date_only = false;
  Civil local;               // as written (in its zone, or UTC for …Z)
  std::string zone;          // IANA name, the TZID as given, "UTC", or "" for floating
  int64_t ms = 0;            // the instant (a date: its midnight in the zone)
};

// The zones a calendar defines itself (its VTIMEZONEs), by TZID.
using Zones = std::map<std::string, std::shared_ptr<const Zone>>;
Zones calendar_zones(const Component& vcalendar, std::vector<std::string>* warnings);

// A property's value as a When: TZID from the calendar's own zones, then IANA, then a Windows
// name; an unknown one, or a floating time, is read in `fallback` (or UTC) with a warning.
std::optional<When> when_of(const Property& p, const Zones& zones, std::string_view fallback,
                            std::vector<std::string>* warnings);
std::optional<When> parse_when(std::string_view value, std::string_view tzid, bool date_only, const Zones& zones,
                               std::string_view fallback, std::vector<std::string>* warnings);
// "19970714T173000Z", "19970714T173000" (with its TZID), "19970714".
std::string when_value(const When& w);

// A DURATION (RFC 5545 § 3.3.6): "PT1H30M", "-P1D", "P2W", in seconds.
std::optional<int64_t> parse_duration(std::string_view v);
std::string duration_text(int64_t seconds);

}  // namespace goober::calendar
