#include "goober/calendar/rrule.h"

#include <algorithm>
#include <set>

#include "goober/base/encoding.h"

namespace goober::calendar {

namespace {

const char* const kDays[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};

std::optional<int> int_of(std::string_view s) {
  if (s.empty() || s.size() > 9) return std::nullopt;
  bool neg = s[0] == '-';
  if (s[0] == '+' || s[0] == '-') s.remove_prefix(1);
  if (s.empty()) return std::nullopt;
  int v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + (c - '0');
  }
  return neg ? -v : v;
}

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t e = s.find(sep, i);
    if (e == std::string_view::npos) e = s.size();
    out.emplace_back(s.substr(i, e - i));
    i = e + 1;
  }
  return out;
}

// A month's day for an ordinal weekday ("2TU", "-1FR"), or all of that weekday's days.
std::vector<int> month_weekdays(int64_t y, int m, int ordinal, int wd) {
  std::vector<int> all;
  int n = days_in_month(y, m);
  for (int d = 1; d <= n; ++d) {
    if (weekday(y, m, d) == wd) all.push_back(d);
  }
  if (ordinal == 0) return all;
  int idx = ordinal > 0 ? ordinal - 1 : int(all.size()) + ordinal;
  if (idx < 0 || idx >= int(all.size())) return {};
  return {all[size_t(idx)]};
}

// The year's days (as day of year, 1-based) for an ordinal weekday, or all of them.
std::vector<int> year_weekdays(int64_t y, int ordinal, int wd) {
  std::vector<int> all;
  int n = leap_year(y) ? 366 : 365;
  int first = weekday(y, 1, 1);
  for (int yd = 1; yd <= n; ++yd) {
    if ((first + yd - 1) % 7 == wd) all.push_back(yd);
  }
  if (ordinal == 0) return all;
  int idx = ordinal > 0 ? ordinal - 1 : int(all.size()) + ordinal;
  if (idx < 0 || idx >= int(all.size())) return {};
  return {all[size_t(idx)]};
}

Civil from_yearday(int64_t y, int yd) {
  int64_t days = days_from_civil(y, 1, 1) + yd - 1;
  int64_t yy;
  unsigned m, d;
  civil_from_days(days, &yy, &m, &d);
  return Civil{yy, int(m), int(d), 0, 0, 0};
}

// The start of week 1 of a year (RFC 5545: the first week with at least four of the year's
// days, weeks starting on wkst), as days since the epoch.
int64_t week_one(int64_t y, int wkst) {
  int64_t jan1 = days_from_civil(y, 1, 1);
  int wd = weekday(y, 1, 1);
  int back = (wd - wkst + 7) % 7;  // days from the week's start to Jan 1
  int64_t start = jan1 - back;
  if (7 - back < 4) start += 7;  // Jan 1's week has fewer than four days of the year
  return start;
}

int weeks_in_year(int64_t y, int wkst) { return int((week_one(y + 1, wkst) - week_one(y, wkst)) / 7); }

bool in(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

bool monthday_fits(const std::vector<int>& bymd, int64_t y, int m, int d) {
  int n = days_in_month(y, m);
  for (int v : bymd) {
    if (v == d || (v < 0 && n + v + 1 == d)) return true;
  }
  return false;
}

bool yearday_fits(const std::vector<int>& byyd, int64_t y, int yd) {
  int n = leap_year(y) ? 366 : 365;
  for (int v : byyd) {
    if (v == yd || (v < 0 && n + v + 1 == yd)) return true;
  }
  return false;
}

bool weekday_fits(const std::vector<std::pair<int, int>>& byday, int wd) {
  for (auto [o, w] : byday) {
    if (w == wd) return true;
  }
  return false;
}

}  // namespace

std::optional<Rule> parse_rule(std::string_view text, std::string* error) {
  auto bad = [&](std::string why) -> std::optional<Rule> {
    if (error) *error = std::move(why);
    return std::nullopt;
  };
  if (text.starts_with("RRULE:") || text.starts_with("rrule:")) text.remove_prefix(6);
  Rule r;
  bool have_freq = false;
  for (const auto& part : split(text, ';')) {
    if (part.empty()) continue;
    auto eq = part.find('=');
    if (eq == std::string::npos) return bad("RRULE part without =: " + part);
    std::string key = part.substr(0, eq);
    for (auto& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
    std::string val = part.substr(eq + 1);
    std::string upper = val;
    for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    auto ints = [&](std::vector<int>* out, int lo, int hi, bool neg) -> bool {
      for (const auto& x : split(val, ',')) {
        auto v = int_of(x);
        if (!v || *v == 0 || std::abs(*v) < lo || std::abs(*v) > hi || (!neg && *v < 0)) return false;
        out->push_back(*v);
      }
      return true;
    };
    if (key == "FREQ") {
      static const std::pair<const char*, Freq> kFreqs[] = {
          {"SECONDLY", Freq::kSecondly}, {"MINUTELY", Freq::kMinutely}, {"HOURLY", Freq::kHourly}, {"DAILY", Freq::kDaily},
          {"WEEKLY", Freq::kWeekly},     {"MONTHLY", Freq::kMonthly},   {"YEARLY", Freq::kYearly}};
      bool found = false;
      for (auto [n, f] : kFreqs) {
        if (upper == n) r.freq = f, found = true;
      }
      if (!found) return bad("FREQ is SECONDLY, MINUTELY, HOURLY, DAILY, WEEKLY, MONTHLY, or YEARLY");
      have_freq = true;
    } else if (key == "INTERVAL") {
      auto v = int_of(val);
      if (!v || *v < 1 || *v > 100000) return bad("INTERVAL is a positive number");
      r.interval = *v;
    } else if (key == "COUNT") {
      auto v = int_of(val);
      if (!v || *v < 1) return bad("COUNT is a positive number");
      r.count = *v;
    } else if (key == "UNTIL") {
      // 19971224T000000Z, 19971224T000000, or 19971224.
      std::string v = upper;
      bool z = !v.empty() && v.back() == 'Z';
      if (z) v.pop_back();
      Civil c;
      bool ok = (v.size() == 8 || (v.size() == 15 && v[8] == 'T'));
      auto num = [&](size_t at, size_t n) { return int_of(v.substr(at, n)).value_or(-1); };
      if (ok) {
        c.y = num(0, 4), c.mo = num(4, 2), c.d = num(6, 2);
        if (v.size() == 15) c.h = num(9, 2), c.mi = num(11, 2), c.s = num(13, 2);
        ok = valid(c);
      }
      if (!ok) return bad("UNTIL is a date or date-time, like 19971224T000000Z");
      if (v.size() == 8) {
        r.until_date = true;
        r.until_local = Civil{c.y, c.mo, c.d, 23, 59, 59};
      } else if (z) {
        r.until_utc_ms = civil_seconds(c) * 1000;
      } else {
        r.until_local = c;
      }
    } else if (key == "BYSECOND") {
      for (const auto& x : split(val, ',')) {
        auto v = int_of(x);
        if (!v || *v < 0 || *v > 60) return bad("BYSECOND is 0 to 60");
        r.bysecond.push_back(*v);
      }
    } else if (key == "BYMINUTE") {
      for (const auto& x : split(val, ',')) {
        auto v = int_of(x);
        if (!v || *v < 0 || *v > 59) return bad("BYMINUTE is 0 to 59");
        r.byminute.push_back(*v);
      }
    } else if (key == "BYHOUR") {
      for (const auto& x : split(val, ',')) {
        auto v = int_of(x);
        if (!v || *v < 0 || *v > 23) return bad("BYHOUR is 0 to 23");
        r.byhour.push_back(*v);
      }
    } else if (key == "BYDAY") {
      for (const auto& x : split(upper, ',')) {
        if (x.size() < 2) return bad("BYDAY is like MO, 2TU, or -1FR");
        std::string day = x.substr(x.size() - 2);
        int wd = -1;
        for (int i = 0; i < 7; ++i) {
          if (day == kDays[i]) wd = i;
        }
        int ord = 0;
        if (x.size() > 2) {
          auto o = int_of(x.substr(0, x.size() - 2));
          if (!o || *o == 0 || std::abs(*o) > 53) return bad("BYDAY's ordinal is 1 to 53, or -1 to -53");
          ord = *o;
        }
        if (wd < 0) return bad("BYDAY is like MO, 2TU, or -1FR");
        r.byday.emplace_back(ord, wd);
      }
    } else if (key == "BYMONTHDAY") {
      if (!ints(&r.bymonthday, 1, 31, true)) return bad("BYMONTHDAY is 1 to 31, or -1 to -31");
    } else if (key == "BYYEARDAY") {
      if (!ints(&r.byyearday, 1, 366, true)) return bad("BYYEARDAY is 1 to 366, or -1 to -366");
    } else if (key == "BYWEEKNO") {
      if (!ints(&r.byweekno, 1, 53, true)) return bad("BYWEEKNO is 1 to 53, or -1 to -53");
    } else if (key == "BYMONTH") {
      if (!ints(&r.bymonth, 1, 12, false)) return bad("BYMONTH is 1 to 12");
    } else if (key == "BYSETPOS") {
      if (!ints(&r.bysetpos, 1, 366, true)) return bad("BYSETPOS is 1 to 366, or -1 to -366");
    } else if (key == "WKST") {
      int wd = -1;
      for (int i = 0; i < 7; ++i) {
        if (upper == kDays[i]) wd = i;
      }
      if (wd < 0) return bad("WKST is MO to SU");
      r.wkst = wd;
    } else if (key.starts_with("X-")) {
      // extensions are ignored
    } else {
      return bad("unknown RRULE part " + key);
    }
  }
  if (!have_freq) return bad("an RRULE needs FREQ");
  if (r.count && (r.until_utc_ms || r.until_local)) return bad("an RRULE has COUNT or UNTIL, not both");
  if (!r.byweekno.empty() && r.freq != Freq::kYearly) return bad("BYWEEKNO only goes with FREQ=YEARLY");
  return r;
}

std::string format_rule(const Rule& r) {
  static const char* kFreq[] = {"SECONDLY", "MINUTELY", "HOURLY", "DAILY", "WEEKLY", "MONTHLY", "YEARLY"};
  std::string out = std::string("FREQ=") + kFreq[int(r.freq)];
  auto list = [&](const char* name, const std::vector<int>& v) {
    if (v.empty()) return;
    out += std::string(";") + name + "=";
    for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + std::to_string(v[i]);
  };
  if (r.interval != 1) out += ";INTERVAL=" + std::to_string(r.interval);
  if (r.count) out += ";COUNT=" + std::to_string(*r.count);
  if (r.until_utc_ms) {
    Civil c = civil_from_seconds(*r.until_utc_ms / 1000);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04lld%02d%02dT%02d%02d%02dZ", (long long)c.y, c.mo, c.d, c.h, c.mi, c.s);
    out += std::string(";UNTIL=") + buf;
  } else if (r.until_local) {
    const Civil& c = *r.until_local;
    char buf[32];
    if (r.until_date) {
      std::snprintf(buf, sizeof buf, "%04lld%02d%02d", (long long)c.y, c.mo, c.d);
    } else {
      std::snprintf(buf, sizeof buf, "%04lld%02d%02dT%02d%02d%02d", (long long)c.y, c.mo, c.d, c.h, c.mi, c.s);
    }
    out += std::string(";UNTIL=") + buf;
  }
  list("BYSECOND", r.bysecond);
  list("BYMINUTE", r.byminute);
  list("BYHOUR", r.byhour);
  if (!r.byday.empty()) {
    out += ";BYDAY=";
    for (size_t i = 0; i < r.byday.size(); ++i) {
      out += (i ? "," : "") + (r.byday[i].first ? std::to_string(r.byday[i].first) : "") + kDays[r.byday[i].second];
    }
  }
  list("BYMONTHDAY", r.bymonthday);
  list("BYYEARDAY", r.byyearday);
  list("BYWEEKNO", r.byweekno);
  list("BYMONTH", r.bymonth);
  list("BYSETPOS", r.bysetpos);
  if (r.wkst != 0) out += std::string(";WKST=") + kDays[r.wkst];
  return out;
}

Expansion expand(const Civil& dtstart, const Rule& r, const Civil& horizon, size_t max,
                 const std::function<int64_t(const Civil&)>& to_utc) {
  Expansion out;
  auto past_until = [&](const Civil& c) {
    if (r.until_utc_ms) return to_utc(c) > *r.until_utc_ms;
    if (r.until_local) return c > *r.until_local;
    return false;
  };
  std::vector<int> hours = r.byhour.empty() ? std::vector<int>{dtstart.h} : r.byhour;
  std::vector<int> minutes = r.byminute.empty() ? std::vector<int>{dtstart.mi} : r.byminute;
  std::vector<int> seconds = r.bysecond.empty() ? std::vector<int>{dtstart.s} : r.bysecond;
  std::sort(hours.begin(), hours.end());
  std::sort(minutes.begin(), minutes.end());
  std::sort(seconds.begin(), seconds.end());
  // The day-level rule parts limit a day (for DAILY and finer), or the day set a period makes.
  auto day_fits = [&](int64_t y, int m, int d) {
    if (!r.bymonth.empty() && !in(r.bymonth, m)) return false;
    if (!r.bymonthday.empty() && !monthday_fits(r.bymonthday, y, m, d)) return false;
    if (!r.byyearday.empty() && !yearday_fits(r.byyearday, y, day_of_year(Civil{y, m, d}))) return false;
    if (!r.byday.empty() && !weekday_fits(r.byday, weekday(y, m, d))) return false;
    return true;
  };
  auto with_times = [&](std::vector<Civil>& set, int64_t y, int m, int d) {
    for (int h : hours) {
      for (int mi : minutes) {
        for (int s : seconds) set.push_back(Civil{y, m, d, h, mi, s});
      }
    }
  };

  size_t emitted = 0;
  bool done = false;
  auto emit = [&](const Civil& c) {
    if (done) return;
    if (c > horizon) {
      done = true;
      out.truncated = true;
      return;
    }
    if (past_until(c)) {
      done = true;
      return;
    }
    if (r.count && int64_t(emitted) >= *r.count) {
      done = true;
      return;
    }
    if (out.instances.size() >= max) {
      done = true;
      out.truncated = true;
      return;
    }
    out.instances.push_back(c);
    ++emitted;
  };
  // DTSTART is always the first instance (RFC 5545 § 3.8.5.3).
  emit(dtstart);

  constexpr int kMaxPeriods = 200000;
  for (int64_t p = 0; p < kMaxPeriods && !done; ++p) {
    std::vector<Civil> set;
    int64_t step = p * r.interval;
    switch (r.freq) {
      case Freq::kYearly: {
        int64_t y = dtstart.y + step;
        if (y > horizon.y + 1) {
          done = true;
          out.truncated = true;
          break;
        }
        std::vector<Civil> days;
        if (!r.byweekno.empty()) {
          int nweeks = weeks_in_year(y, r.wkst);
          int64_t w1 = week_one(y, r.wkst);
          for (int wn : r.byweekno) {
            int w = wn > 0 ? wn : nweeks + wn + 1;
            if (w < 1 || w > nweeks) continue;
            for (int k = 0; k < 7; ++k) {
              int64_t yy;
              unsigned m, d;
              civil_from_days(w1 + int64_t(w - 1) * 7 + k, &yy, &m, &d);
              int wd = weekday(yy, int(m), int(d));
              bool keep = r.byday.empty() ? wd == weekday(dtstart.y, dtstart.mo, dtstart.d) : weekday_fits(r.byday, wd);
              if (keep && (r.bymonth.empty() || in(r.bymonth, int(m)))) days.push_back(Civil{yy, int(m), int(d)});
            }
          }
        } else if (!r.byyearday.empty()) {
          int n = leap_year(y) ? 366 : 365;
          for (int v : r.byyearday) {
            int yd = v > 0 ? v : n + v + 1;
            if (yd < 1 || yd > n) continue;
            Civil c = from_yearday(y, yd);
            if ((r.bymonth.empty() || in(r.bymonth, c.mo)) && (r.byday.empty() || weekday_fits(r.byday, weekday(c.y, c.mo, c.d))) &&
                (r.bymonthday.empty() || monthday_fits(r.bymonthday, c.y, c.mo, c.d))) {
              days.push_back(c);
            }
          }
        } else {
          std::vector<int> months = r.bymonth;
          bool any_day_rule = !r.bymonthday.empty() || !r.byday.empty();
          if (months.empty()) {
            if (any_day_rule) {
              for (int m = 1; m <= 12; ++m) months.push_back(m);
            } else {
              months.push_back(dtstart.mo);
            }
          }
          if (!r.byday.empty() && r.bymonth.empty() && r.bymonthday.empty()) {
            // BYDAY across the year: ordinals count within the year.
            std::set<int> yds;
            for (auto [o, wd] : r.byday) {
              for (int yd : year_weekdays(y, o, wd)) yds.insert(yd);
            }
            for (int yd : yds) days.push_back(from_yearday(y, yd));
          } else {
            for (int m : months) {
              if (!r.bymonthday.empty()) {
                int n = days_in_month(y, m);
                std::set<int> ds;
                for (int v : r.bymonthday) {
                  int d = v > 0 ? v : n + v + 1;
                  if (d >= 1 && d <= n && (r.byday.empty() || weekday_fits(r.byday, weekday(y, m, d)))) ds.insert(d);
                }
                for (int d : ds) days.push_back(Civil{y, m, d});
              } else if (!r.byday.empty()) {
                std::set<int> ds;
                for (auto [o, wd] : r.byday) {
                  for (int d : month_weekdays(y, m, o, wd)) ds.insert(d);
                }
                for (int d : ds) days.push_back(Civil{y, m, d});
              } else if (dtstart.d <= days_in_month(y, m)) {
                days.push_back(Civil{y, m, dtstart.d});
              }
            }
          }
        }
        for (const auto& d : days) with_times(set, d.y, d.mo, d.d);
        break;
      }
      case Freq::kMonthly: {
        int64_t months = (dtstart.y * 12 + dtstart.mo - 1) + step;
        int64_t y = months / 12;
        int m = int(months % 12) + 1;
        if (y > horizon.y + 1) {
          done = true;
          out.truncated = true;
          break;
        }
        if (!r.bymonth.empty() && !in(r.bymonth, m)) break;
        int n = days_in_month(y, m);
        std::set<int> ds;
        if (!r.bymonthday.empty()) {
          for (int v : r.bymonthday) {
            int d = v > 0 ? v : n + v + 1;
            if (d >= 1 && d <= n && (r.byday.empty() || weekday_fits(r.byday, weekday(y, m, d)))) ds.insert(d);
          }
        } else if (!r.byday.empty()) {
          for (auto [o, wd] : r.byday) {
            for (int d : month_weekdays(y, m, o, wd)) ds.insert(d);
          }
        } else if (dtstart.d <= n) {
          ds.insert(dtstart.d);
        }
        for (int d : ds) {
          if (r.byyearday.empty() || yearday_fits(r.byyearday, y, day_of_year(Civil{y, m, d}))) with_times(set, y, m, d);
        }
        break;
      }
      case Freq::kWeekly: {
        int64_t start_days = days_from_civil(dtstart.y, unsigned(dtstart.mo), unsigned(dtstart.d));
        int back = (weekday(dtstart.y, dtstart.mo, dtstart.d) - r.wkst + 7) % 7;
        int64_t week = start_days - back + step * 7;
        int64_t yy;
        unsigned mm, dd;
        civil_from_days(week, &yy, &mm, &dd);
        if (yy > horizon.y + 1) {
          done = true;
          out.truncated = true;
          break;
        }
        for (int k = 0; k < 7; ++k) {
          civil_from_days(week + k, &yy, &mm, &dd);
          int wd = weekday(yy, int(mm), int(dd));
          bool keep = r.byday.empty() ? wd == weekday(dtstart.y, dtstart.mo, dtstart.d) : weekday_fits(r.byday, wd);
          if (keep && (r.bymonth.empty() || in(r.bymonth, int(mm)))) with_times(set, yy, int(mm), int(dd));
        }
        break;
      }
      case Freq::kDaily: {
        int64_t days = days_from_civil(dtstart.y, unsigned(dtstart.mo), unsigned(dtstart.d)) + step;
        int64_t yy;
        unsigned mm, dd;
        civil_from_days(days, &yy, &mm, &dd);
        if (yy > horizon.y + 1) {
          done = true;
          out.truncated = true;
          break;
        }
        if (day_fits(yy, int(mm), int(dd))) with_times(set, yy, int(mm), int(dd));
        break;
      }
      case Freq::kHourly:
      case Freq::kMinutely:
      case Freq::kSecondly: {
        int64_t unit = r.freq == Freq::kHourly ? 3600 : r.freq == Freq::kMinutely ? 60 : 1;
        int64_t secs = civil_seconds(dtstart) + step * unit;
        Civil c = civil_from_seconds(secs);
        if (c > horizon) {
          done = true;
          out.truncated = true;
          break;
        }
        if (!day_fits(c.y, c.mo, c.d)) break;
        if (r.freq == Freq::kHourly) {
          if (!r.byhour.empty() && !in(r.byhour, c.h)) break;
          for (int mi : minutes) {
            for (int s : seconds) set.push_back(Civil{c.y, c.mo, c.d, c.h, mi, s});
          }
        } else if (r.freq == Freq::kMinutely) {
          if ((!r.byhour.empty() && !in(r.byhour, c.h)) || (!r.byminute.empty() && !in(r.byminute, c.mi))) break;
          for (int s : seconds) set.push_back(Civil{c.y, c.mo, c.d, c.h, c.mi, s});
        } else {
          if ((!r.byhour.empty() && !in(r.byhour, c.h)) || (!r.byminute.empty() && !in(r.byminute, c.mi)) ||
              (!r.bysecond.empty() && !in(r.bysecond, c.s))) {
            break;
          }
          set.push_back(c);
        }
        break;
      }
    }
    if (done) break;
    std::sort(set.begin(), set.end());
    set.erase(std::unique(set.begin(), set.end()), set.end());
    std::erase_if(set, [](const Civil& c) { return !valid(c); });
    if (!r.bysetpos.empty()) {
      std::vector<Civil> picked;
      for (int pos : r.bysetpos) {
        int idx = pos > 0 ? pos - 1 : int(set.size()) + pos;
        if (idx >= 0 && idx < int(set.size())) picked.push_back(set[size_t(idx)]);
      }
      std::sort(picked.begin(), picked.end());
      picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
      set = std::move(picked);
    }
    for (const auto& c : set) {
      if (c <= dtstart) continue;  // dtstart is already the first
      emit(c);
      if (done) break;
    }
    if (p == kMaxPeriods - 1) out.truncated = true;
  }
  return out;
}

}  // namespace goober::calendar
