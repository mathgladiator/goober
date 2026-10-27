#include "goober/timers/cron.h"

#include <vector>

namespace goober::timers {

namespace {

using namespace std::chrono;

bool parse_number(std::string_view s, int lo, int hi, int* out) {
  if (s.empty() || s.size() > 2) return false;
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  if (v < lo || v > hi) return false;
  *out = v;
  return true;
}

// One field into a bit set; `wrap7` folds day-of-week 7 onto 0.
bool parse_field(std::string_view field, int lo, int hi, uint64_t* bits, bool* any, std::string* error,
                 const char* name, bool wrap7 = false) {
  *bits = 0;
  *any = field == "*";
  size_t start = 0;
  while (start <= field.size()) {
    size_t comma = field.find(',', start);
    std::string_view part = field.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    int step = 1;
    if (auto slash = part.find('/'); slash != std::string_view::npos) {
      if (!parse_number(part.substr(slash + 1), 1, hi, &step)) return *error = std::string("bad step in ") + name, false;
      part = part.substr(0, slash);
    }
    int a = lo, b = hi;
    if (part != "*") {
      if (auto dash = part.find('-'); dash != std::string_view::npos) {
        if (!parse_number(part.substr(0, dash), lo, hi, &a) || !parse_number(part.substr(dash + 1), lo, hi, &b) || a > b) {
          return *error = std::string("bad range in ") + name, false;
        }
      } else {
        if (!parse_number(part, lo, hi, &a)) return *error = std::string("bad value in ") + name, false;
        b = step > 1 ? hi : a;
      }
    }
    for (int v = a; v <= b; v += step) *bits |= uint64_t(1) << (wrap7 && v == 7 ? 0 : v);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return *bits != 0 || (*error = std::string("empty ") + name, false);
}

}  // namespace

std::optional<Cron> Cron::parse(std::string_view text, std::string* error) {
  std::vector<std::string_view> f;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t j = i;
    while (j < text.size() && text[j] != ' ') ++j;
    if (j > i) f.push_back(text.substr(i, j - i));
    i = j;
  }
  if (f.size() != 5) return *error = "a cron schedule has five fields: minute hour day month weekday", std::nullopt;
  Cron c;
  c.text_ = std::string(text);
  uint64_t bits;
  bool any;
  if (!parse_field(f[0], 0, 59, &bits, &any, error, "minute")) return std::nullopt;
  c.minutes_ = bits;
  if (!parse_field(f[1], 0, 23, &bits, &any, error, "hour")) return std::nullopt;
  c.hours_ = uint32_t(bits);
  if (!parse_field(f[2], 1, 31, &bits, &c.any_day_, error, "day of month")) return std::nullopt;
  c.days_ = uint32_t(bits);
  if (!parse_field(f[3], 1, 12, &bits, &any, error, "month")) return std::nullopt;
  c.months_ = uint16_t(bits);
  if (!parse_field(f[4], 0, 7, &bits, &c.any_weekday_, error, "day of week", true)) return std::nullopt;
  c.weekdays_ = uint8_t(bits);
  return c;
}

bool Cron::day_matches(year_month_day ymd) const {
  if (!(months_ & (1u << unsigned(ymd.month())))) return false;
  bool dom = days_ & (1u << unsigned(ymd.day()));
  bool dow = weekdays_ & (1u << weekday(ymd).c_encoding());
  if (any_day_ && any_weekday_) return true;
  if (any_day_) return dow;
  if (any_weekday_) return dom;
  return dom || dow;
}

std::optional<int64_t> Cron::next(int64_t after_ms, const time_zone* zone) const {
  if (!zone) return std::nullopt;
  auto after = sys_time<milliseconds>(milliseconds(after_ms));
  auto local_after = zone->to_local(floor<minutes>(after));
  auto start = local_after + minutes(1);
  auto day = floor<days>(start);
  for (int n = 0; n < 366 * 8; ++n, day += days(1)) {
    year_month_day ymd(day);
    if (!day_matches(ymd)) continue;
    for (int h = 0; h < 24; ++h) {
      if (!(hours_ & (1u << h))) continue;
      for (int m = 0; m < 60; ++m) {
        if (!(minutes_ & (uint64_t(1) << m))) continue;
        auto candidate = local_time<minutes>(day + hours(h) + minutes(m));
        if (candidate < start) continue;
        // A local time that doesn't exist maps to the transition: the next time that does.
        auto sys = zone->to_sys(candidate, choose::earliest);
        if (sys > after) return duration_cast<milliseconds>(sys.time_since_epoch()).count();
      }
    }
  }
  return std::nullopt;
}

std::optional<int64_t> Cron::previous(int64_t at_ms, const time_zone* zone) const {
  if (!zone) return std::nullopt;
  auto at = sys_time<milliseconds>(milliseconds(at_ms));
  auto local_at = zone->to_local(floor<minutes>(at));
  auto day = floor<days>(local_at);
  for (int n = 0; n < 367; ++n, day -= days(1)) {
    year_month_day ymd(day);
    if (!day_matches(ymd)) continue;
    for (int h = 23; h >= 0; --h) {
      if (!(hours_ & (1u << h))) continue;
      for (int m = 59; m >= 0; --m) {
        if (!(minutes_ & (uint64_t(1) << m))) continue;
        auto candidate = local_time<minutes>(day + hours(h) + minutes(m));
        if (candidate > local_at) continue;
        auto sys = zone->to_sys(candidate, choose::earliest);
        if (sys <= at) return duration_cast<milliseconds>(sys.time_since_epoch()).count();
      }
    }
  }
  return std::nullopt;
}

std::optional<int64_t> parse_instant_ms(std::string_view t) {
  if (t.empty() || t.size() > 40) return std::nullopt;
  bool digits_only = true;
  for (char c : t) digits_only = digits_only && c >= '0' && c <= '9';
  if (digits_only) {
    if (t.size() > 15) return std::nullopt;
    return std::stoll(std::string(t));
  }
  auto num = [&](size_t at, size_t n, int* out) {
    if (at + n > t.size()) return false;
    int v = 0;
    for (size_t i = at; i < at + n; ++i) {
      if (t[i] < '0' || t[i] > '9') return false;
      v = v * 10 + (t[i] - '0');
    }
    *out = v;
    return true;
  };
  int y, mo, d, h, mi, se;
  if (t.size() < 20 || t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' || t[16] != ':' || t.back() != 'Z' ||
      !num(0, 4, &y) || !num(5, 2, &mo) || !num(8, 2, &d) || !num(11, 2, &h) || !num(14, 2, &mi) || !num(17, 2, &se)) {
    return std::nullopt;
  }
  int ms = 0;
  if (t.size() > 20) {
    if (t[19] != '.' || t.size() > 31) return std::nullopt;
    std::string frac(t.substr(20, t.size() - 21));
    if (frac.empty()) return std::nullopt;
    for (char c : frac) {
      if (c < '0' || c > '9') return std::nullopt;
    }
    frac.resize(3, '0');
    ms = std::stoi(frac.substr(0, 3));
  }
  year_month_day ymd{year(y), month(unsigned(mo)), day(unsigned(d))};
  if (!ymd.ok() || h > 23 || mi > 59 || se > 60) return std::nullopt;
  auto tp = sys_days(ymd) + hours(h) + minutes(mi) + seconds(se) + milliseconds(ms);
  return duration_cast<milliseconds>(tp.time_since_epoch()).count();
}

const time_zone* find_zone(std::string_view name) {
  if (name.empty() || name.size() > 64) return nullptr;
  try {
    return locate_zone(name);
  } catch (...) {
    return nullptr;
  }
}

const time_zone* host_zone() {
  try {
    return current_zone();
  } catch (...) {
    return locate_zone("UTC");
  }
}

}  // namespace goober::timers
