#pragma once

// What time@1 and ics@1 share: argument reading, instants and wall-clock times as JSON, and
// schema helpers for their contracts.

#include <optional>
#include <string>

#include "goober/calendar/zones.h"
#include "goober/libraries/libraries.h"

namespace goober::libraries::detail {

[[noreturn]] inline void bad(std::string message) { throw Error{"bad_input", std::move(message)}; }

std::string text(const bj::object& o, std::string_view k, bool required = false);
std::optional<int64_t> integer(const bj::object& o, std::string_view k);
const bj::object* object(const bj::object& o, std::string_view k);

// An IANA zone by name, or bad_input; ctx records that tzdata was read.
std::shared_ptr<const calendar::Zone> zone_of(const std::string& name, Context& ctx);

// "2026-11-06T00:00:00Z" (with .sss when there are milliseconds).
std::string utc_text(int64_t ms);
// An instant: unix ms, or RFC 3339 text with Z or an offset.
std::optional<int64_t> instant(const bj::value& v);
int64_t instant_field(const bj::object& o, std::string_view k);  // required, or bad_input
// A wall-clock time: "2026-11-05T18:00:00", or {"date", "time"}.
std::optional<calendar::Civil> local_of(const bj::value& v, bool* date_only);

// {ms, utc, local, zone, offset} of an instant in a zone.
bj::object at_zone(int64_t ms, const calendar::Zone& z);

bj::object schema(std::initializer_list<std::pair<const char*, bj::value>> props, std::initializer_list<const char*> required);
bj::value str(const char* description);
bj::value num(const char* description);
bj::value any(const char* description);

}  // namespace goober::libraries::detail
