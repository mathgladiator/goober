// time@1 (design/machine/831.Libraries.md § 4).

#include <algorithm>

#include "common.h"
#include "goober/calendar/rrule.h"

namespace goober::libraries {

using namespace detail;
namespace cal = calendar;

namespace {

constexpr int64_t kMaxInstances = 10000;
const char* const kWeekdays[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};

// {local, zone} or {ms, zone}: an instant and its zone.
std::pair<int64_t, std::shared_ptr<const cal::Zone>> moment(const bj::object& in, Context& ctx, const char* field) {
  auto zone = zone_of(text(in, "zone", true), ctx);
  if (auto* l = in.if_contains("local")) {
    bool date_only = false;
    auto c = local_of(*l, &date_only);
    if (!c) bad("local is a wall-clock time like 2026-11-05T18:00:00");
    return {zone->to_utc(*c, nullptr), zone};
  }
  return {instant_field(in, field), zone};
}

int week_start(const bj::object& in) {
  std::string w = text(in, "week_starts");
  if (w.empty()) return 0;
  std::transform(w.begin(), w.end(), w.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (int i = 0; i < 7; ++i) {
    if (w == kWeekdays[i] || w == std::string(kWeekdays[i]).substr(0, 2) || w == std::string(kWeekdays[i]).substr(0, 3)) return i;
  }
  bad("week_starts is a weekday, like monday or sunday");
}

cal::Civil add_months(cal::Civil c, int64_t months) {
  int64_t total = c.y * 12 + (c.mo - 1) + months;
  c.y = total / 12;
  c.mo = int(total % 12) + 1;
  c.d = std::min(c.d, cal::days_in_month(c.y, c.mo));  // Jan 31 + 1 month: the end of February
  return c;
}

cal::Civil add_days(cal::Civil c, int64_t days) {
  int64_t n = cal::days_from_civil(c.y, unsigned(c.mo), unsigned(c.d)) + days;
  unsigned m, d;
  cal::civil_from_days(n, &c.y, &m, &d);
  c.mo = int(m), c.d = int(d);
  return c;
}

bj::object fmt(int64_t ms, const cal::Zone& z) {
  auto info = z.at(ms);
  auto local = z.to_local(ms);
  int wd = cal::weekday(local.y, local.mo, local.d);
  // ISO 8601 week: the week (Monday first) holding the year's first Thursday is week 1.
  int64_t days = cal::days_from_civil(local.y, unsigned(local.mo), unsigned(local.d));
  int64_t thursday = days - wd + 3;
  int64_t ty;
  unsigned tm, td;
  cal::civil_from_days(thursday, &ty, &tm, &td);
  int iso_week = int((thursday - cal::days_from_civil(ty, 1, 1)) / 7) + 1;
  char tbuf[16];
  std::snprintf(tbuf, sizeof tbuf, "%02d:%02d:%02d", local.h, local.mi, local.s);
  return bj::object{{"ms", ms},
                    {"utc", utc_text(ms)},
                    {"local", cal::civil_text(local)},
                    {"date", cal::civil_text(local, true)},
                    {"time", tbuf},
                    {"zone", z.name()},
                    {"offset", cal::offset_text(info.offset_s)},
                    {"abbreviation", info.abbreviation},
                    {"dst", info.dst},
                    {"weekday", kWeekdays[wd]},
                    {"iso_week", std::to_string(ty) + "-W" + (iso_week < 10 ? "0" : "") + std::to_string(iso_week)},
                    {"day_of_year", cal::day_of_year(local)}};
}

void tz(bj::object& out) { out["tzdata"] = cal::tzdata_version(); }

}  // namespace

Library make_time() {
  Library lib;
  lib.name = "time";
  lib.major = 1;
  lib.description = "Time zones, calendar arithmetic, and recurrence, with the server's IANA tzdata (named in each answer).";
  auto& f = lib.functions;

  f.push_back({"now", "The request's clock: the same instant goober_now_ms gives, so a replay sees the same now.",
               schema({{"zone", str("An IANA zone to show it in, like America/Chicago.")}}, {}),
               schema({{"ms", num("Unix milliseconds.")}, {"utc", str("RFC 3339 in UTC.")}}, {"ms", "utc"}),
               bj::array{bj::object{{"in", bj::object{{"zone", "America/Chicago"}}},
                                    {"out", "{ms, utc, local, zone, offset, tzdata}"}}},
               [](const bj::object& in, Context& ctx) {
                 std::string zn = text(in, "zone");
                 if (zn.empty()) return bj::object{{"ms", ctx.now_ms}, {"utc", utc_text(ctx.now_ms)}};
                 auto out = at_zone(ctx.now_ms, *zone_of(zn, ctx));
                 tz(out);
                 return out;
               }});

  f.push_back({"parse", "Text to an instant: RFC 3339 with Z or an offset; or a date or wall-clock time with zone.",
               schema({{"text", str("Like 2026-11-05T18:00:00-06:00, or 2026-11-05T18:00 with zone.")},
                       {"zone", str("The zone for text without one.")}},
                      {"text"}),
               schema({{"ms", num("Unix milliseconds.")}, {"utc", str("RFC 3339 in UTC.")}}, {"ms", "utc"}),
               bj::array{bj::object{{"in", bj::object{{"text", "2026-11-05T18:00"}, {"zone", "America/Chicago"}}},
                                    {"out", bj::object{{"ms", 1793923200000}, {"utc", "2026-11-06T00:00:00Z"}}}}},
               [](const bj::object& in, Context& ctx) {
                 std::string t = text(in, "text", true);
                 if (auto ms = instant(bj::value(t))) return bj::object{{"ms", *ms}, {"utc", utc_text(*ms)}};
                 bool date_only = false;
                 auto c = cal::parse_civil_text(t, &date_only);
                 if (!c) bad("text is RFC 3339, a date (2026-11-05), or a wall-clock time (2026-11-05T18:00)");
                 std::string zn = text(in, "zone");
                 if (zn.empty()) bad("text has no zone or offset: give zone");
                 int adjusted = 0;
                 int64_t ms = zone_of(zn, ctx)->to_utc(*c, &adjusted);
                 bj::object out{{"ms", ms}, {"utc", utc_text(ms)}};
                 if (adjusted) out["adjusted"] = adjusted == 1 ? "gap" : "overlap";
                 tz(out);
                 return out;
               }});

  f.push_back({"format", "An instant in a zone, in parts: local, date, time, offset, abbreviation, dst, weekday, ISO week.",
               schema({{"ms", any("Unix milliseconds, or RFC 3339 text.")}, {"zone", str("An IANA zone.")}}, {"ms", "zone"}),
               schema({{"local", str("2026-11-05T18:00:00")}, {"weekday", str("thursday")}}, {}),
               bj::array{bj::object{{"in", bj::object{{"ms", 1793923200000}, {"zone", "America/Chicago"}}},
                                    {"out", "{local: 2026-11-05T18:00:00, date, time: 18:00:00, offset: -06:00, "
                                            "abbreviation: CST, dst: false, weekday: thursday, iso_week: 2026-W45, ...}"}}},
               [](const bj::object& in, Context& ctx) {
                 auto out = fmt(instant_field(in, "ms"), *zone_of(text(in, "zone", true), ctx));
                 tz(out);
                 return out;
               }});

  f.push_back({"to_utc", "A wall-clock time in a zone to an instant. In a gap, the time after it; in an overlap, the earlier.",
               schema({{"local", any("2026-11-05T18:00:00, or {date, time}.")}, {"zone", str("An IANA zone.")}}, {"local", "zone"}),
               schema({{"ms", num("Unix milliseconds.")}, {"adjusted", str("gap or overlap, when it was.")}}, {"ms"}), {},
               [](const bj::object& in, Context& ctx) {
                 bool date_only = false;
                 auto* l = in.if_contains("local");
                 auto c = l ? local_of(*l, &date_only) : std::nullopt;
                 if (!c) bad("local is a wall-clock time like 2026-11-05T18:00:00");
                 auto z = zone_of(text(in, "zone", true), ctx);
                 int adjusted = 0;
                 int64_t ms = z->to_utc(*c, &adjusted);
                 bj::object out{{"ms", ms}, {"utc", utc_text(ms)}, {"offset", cal::offset_text(z->at(ms).offset_s)}};
                 if (adjusted) out["adjusted"] = adjusted == 1 ? "gap" : "overlap";
                 tz(out);
                 return out;
               }});

  f.push_back({"add", "Calendar units in a zone (a day across DST is 23 or 25 hours; Jan 31 + 1 month is the end of February), "
                      "then exact hours, minutes, seconds.",
               schema({{"ms", any("The start: unix ms or RFC 3339.")}, {"local", any("Or a wall-clock start.")},
                       {"zone", str("An IANA zone.")}, {"years", num("")}, {"months", num("")}, {"weeks", num("")},
                       {"days", num("")}, {"hours", num("")}, {"minutes", num("")}, {"seconds", num("")}},
                      {"zone"}),
               schema({{"ms", num("")}, {"local", str("")}}, {"ms"}),
               bj::array{bj::object{{"in", bj::object{{"local", "2026-03-07T09:00:00"}, {"zone", "America/Chicago"}, {"days", 1}}},
                                    {"out", "{local: 2026-03-08T09:00:00, ...} (23 hours later)"}}},
               [](const bj::object& in, Context& ctx) {
                 auto [ms, z] = moment(in, ctx, "ms");
                 auto v = [&](const char* k) { return integer(in, k).value_or(0); };
                 int64_t months = v("years") * 12 + v("months"), days = v("weeks") * 7 + v("days");
                 if (std::abs(months) > 120000 || std::abs(days) > 4000000) bad("that's past the calendar");
                 if (months || days) {
                   auto local = z->to_local(ms);
                   int64_t frac = ms % 1000;
                   local = add_days(add_months(local, months), days);
                   ms = z->to_utc(local, nullptr) + frac;
                 }
                 ms += (v("hours") * 3600 + v("minutes") * 60 + v("seconds")) * 1000;
                 auto out = at_zone(ms, *z);
                 tz(out);
                 return out;
               }});

  f.push_back({"diff", "From one instant to another: exact milliseconds and seconds, and with a zone, calendar days.",
               schema({{"from", any("Unix ms or RFC 3339.")}, {"to", any("Unix ms or RFC 3339.")}, {"zone", str("For days.")}},
                      {"from", "to"}),
               schema({{"ms", num("")}, {"seconds", num("")}, {"days", num("Calendar days between the dates.")}}, {"ms"}), {},
               [](const bj::object& in, Context& ctx) {
                 int64_t a = instant_field(in, "from"), b = instant_field(in, "to");
                 bj::object out{{"ms", b - a}, {"seconds", (b - a) / 1000}};
                 if (std::string zn = text(in, "zone"); !zn.empty()) {
                   auto z = zone_of(zn, ctx);
                   auto la = z->to_local(a), lb = z->to_local(b);
                   out["days"] = cal::days_from_civil(lb.y, unsigned(lb.mo), unsigned(lb.d)) -
                                 cal::days_from_civil(la.y, unsigned(la.mo), unsigned(la.d));
                   tz(out);
                 }
                 return out;
               }});

  f.push_back({"start_of", "The start of the day, week, month, or year holding an instant, in a zone.",
               schema({{"ms", any("Unix ms or RFC 3339.")}, {"zone", str("")}, {"unit", str("day, week, month, or year.")},
                       {"week_starts", str("monday (default) through sunday.")}},
                      {"ms", "zone", "unit"}),
               schema({{"ms", num("")}, {"local", str("")}}, {"ms"}), {},
               [](const bj::object& in, Context& ctx) {
                 auto [ms, z] = moment(in, ctx, "ms");
                 auto l = z->to_local(ms);
                 std::string unit = text(in, "unit", true);
                 cal::Civil s{l.y, l.mo, l.d};
                 if (unit == "week") {
                   int back = (cal::weekday(l.y, l.mo, l.d) - week_start(in) + 7) % 7;
                   s = add_days(s, -back);
                 } else if (unit == "month") {
                   s.d = 1;
                 } else if (unit == "year") {
                   s.mo = 1, s.d = 1;
                 } else if (unit != "day") {
                   bad("unit is day, week, month, or year");
                 }
                 auto out = at_zone(z->to_utc(s, nullptr), *z);
                 tz(out);
                 return out;
               }});

  f.push_back({"zone", "A zone's offset, abbreviation, and daylight saving at an instant (now by default), and its next change.",
               schema({{"zone", str("An IANA zone.")}, {"ms", any("Unix ms or RFC 3339; default now.")}}, {"zone"}),
               schema({{"offset", str("-06:00")}, {"next_change", any("")}}, {"offset"}), {},
               [](const bj::object& in, Context& ctx) {
                 auto z = zone_of(text(in, "zone", true), ctx);
                 int64_t ms = in.contains("ms") ? instant_field(in, "ms") : ctx.now_ms;
                 auto info = z->at(ms);
                 bj::object out{{"zone", z->name()},
                                {"offset", cal::offset_text(info.offset_s)},
                                {"offset_seconds", info.offset_s},
                                {"abbreviation", info.abbreviation},
                                {"dst", info.dst}};
                 if (auto next = z->next_change(ms)) out["next_change"] = at_zone(*next, *z);
                 tz(out);
                 return out;
               }});

  f.push_back({"zones", "The IANA zone names the host knows.",
               schema({{"prefix", str("Only names starting with this, like America/.")}}, {}),
               schema({{"zones", any("Names.")}}, {"zones"}), {},
               [](const bj::object& in, Context& ctx) {
                 ctx.used_tzdata = true;
                 std::string prefix = text(in, "prefix");
                 bj::array names;
                 for (auto& n : cal::zone_names()) {
                   if (n.starts_with(prefix)) names.push_back(bj::value(n));
                 }
                 bj::object out{{"zones", std::move(names)}};
                 tz(out);
                 return out;
               }});

  f.push_back({"recur", "An RFC 5545 recurrence rule's instances in a window, on the wall clock of the start's zone.",
               schema({{"rrule", str("Like FREQ=WEEKLY;BYDAY=TH.")}, {"start", any("{local, zone}, or {ms, zone}.")},
                       {"from", any("Only instances at or after this.")}, {"to", any("Only instances before this.")},
                       {"limit", num("At most this many: default 100, at most 10000.")},
                       {"exdates", any("Instants or local times to skip.")}, {"rdates", any("Extra instants or local times.")}},
                      {"rrule", "start"}),
               schema({{"instances", any("[{ms, utc, local}]")}, {"truncated", any("")}}, {"instances"}),
               bj::array{bj::object{{"in", bj::object{{"rrule", "FREQ=WEEKLY;BYDAY=TH;COUNT=3"},
                                                      {"start", bj::object{{"local", "2026-11-05T19:00:00"}, {"zone", "America/Chicago"}}}}},
                                    {"out", "{instances: [Nov 5, Nov 12, Nov 19 at 19:00 Chicago], truncated: false}"}}},
               [](const bj::object& in, Context& ctx) {
                 const auto* start = object(in, "start");
                 if (!start) bad("start is {local, zone} or {ms, zone}");
                 auto [ms0, z] = moment(*start, ctx, "ms");
                 cal::Civil dtstart = z->to_local(ms0);
                 std::string err;
                 auto rule = cal::parse_rule(text(in, "rrule", true), &err);
                 if (!rule) bad("rrule: " + err);
                 int64_t from = in.contains("from") ? instant_field(in, "from") : INT64_MIN;
                 int64_t to = in.contains("to") ? instant_field(in, "to") : ms0 + 50LL * 365 * 86400000;
                 int64_t limit = std::clamp<int64_t>(integer(in, "limit").value_or(100), 1, kMaxInstances);
                 // Exceptions and extras, as instants.
                 auto instants = [&](const char* k) {
                   std::vector<int64_t> out;
                   if (auto* a = in.if_contains(k)) {
                     if (!a->is_array()) bad(std::string(k) + " is a list");
                     for (const auto& v : a->as_array()) {
                       if (auto i = instant(v)) {
                         out.push_back(*i);
                       } else {
                         bool d = false;
                         auto c = local_of(v, &d);
                         if (!c) bad(std::string(k) + " holds instants or local times");
                         out.push_back(z->to_utc(*c, nullptr));
                       }
                     }
                   }
                   return out;
                 };
                 auto ex = instants("exdates"), extra = instants("rdates");
                 cal::Civil horizon = z->to_local(to);
                 auto e = cal::expand(dtstart, *rule, horizon, size_t(kMaxInstances),
                                      [&](const cal::Civil& c) { return z->to_utc(c, nullptr); });
                 std::vector<int64_t> all;
                 for (const auto& c : e.instances) all.push_back(z->to_utc(c, nullptr));
                 for (int64_t r : extra) all.push_back(r);
                 std::sort(all.begin(), all.end());
                 all.erase(std::unique(all.begin(), all.end()), all.end());
                 bj::array out;
                 bool truncated = false;
                 for (int64_t i : all) {
                   if (i < from || i >= to || std::find(ex.begin(), ex.end(), i) != ex.end()) continue;
                   if (int64_t(out.size()) >= limit) {
                     truncated = true;
                     break;
                   }
                   out.push_back(at_zone(i, *z));
                 }
                 // Truncated: more instances than limit in the window, or expansion hit its cap.
                 bj::object res{{"instances", std::move(out)},
                                {"truncated", truncated || int64_t(e.instances.size()) >= kMaxInstances}};
                 tz(res);
                 return res;
               }});
  return lib;
}

}  // namespace goober::libraries
