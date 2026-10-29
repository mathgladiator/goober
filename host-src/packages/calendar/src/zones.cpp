#include "goober/calendar/zones.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

#include "goober/calendar/rrule.h"

namespace goober::calendar {

namespace ch = std::chrono;

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}

void civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = int64_t(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yy + (*m <= 2);
}

bool leap_year(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int64_t y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  return m == 2 && leap_year(y) ? 29 : kDays[m - 1];
}

int day_of_year(const Civil& c) {
  return int(days_from_civil(c.y, unsigned(c.mo), unsigned(c.d)) - days_from_civil(c.y, 1, 1)) + 1;
}

int weekday(int64_t y, int m, int d) {
  int64_t days = days_from_civil(y, unsigned(m), unsigned(d));
  return int(((days % 7) + 7 + 3) % 7);  // 1970-01-01 was a Thursday (3)
}

bool valid(const Civil& c) {
  return c.y >= 1 && c.y <= 9999 && c.mo >= 1 && c.mo <= 12 && c.d >= 1 && c.d <= days_in_month(c.y, c.mo) && c.h >= 0 &&
         c.h <= 23 && c.mi >= 0 && c.mi <= 59 && c.s >= 0 && c.s <= 60;
}

int64_t civil_seconds(const Civil& c) {
  return ((days_from_civil(c.y, unsigned(c.mo), unsigned(c.d)) * 24 + c.h) * 60 + c.mi) * 60 + c.s;
}

Civil civil_from_seconds(int64_t s) {
  int64_t days = s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
  int64_t rest = s - days * 86400;
  Civil c;
  unsigned m, d;
  civil_from_days(days, &c.y, &m, &d);
  c.mo = int(m), c.d = int(d);
  c.h = int(rest / 3600), c.mi = int(rest % 3600 / 60), c.s = int(rest % 60);
  return c;
}

std::string civil_text(const Civil& c, bool date_only) {
  char buf[40];
  if (date_only) {
    std::snprintf(buf, sizeof buf, "%04lld-%02d-%02d", (long long)c.y, c.mo, c.d);
  } else {
    std::snprintf(buf, sizeof buf, "%04lld-%02d-%02dT%02d:%02d:%02d", (long long)c.y, c.mo, c.d, c.h, c.mi, c.s);
  }
  return buf;
}

std::optional<Civil> parse_civil_text(std::string_view t, bool* date_only) {
  auto num = [&](size_t at, size_t n) -> int64_t {
    if (at + n > t.size()) return -1;
    int64_t v = 0;
    for (size_t i = at; i < at + n; ++i) {
      if (t[i] < '0' || t[i] > '9') return -1;
      v = v * 10 + (t[i] - '0');
    }
    return v;
  };
  if (t.size() < 10 || t[4] != '-' || t[7] != '-') return std::nullopt;
  Civil c{num(0, 4), int(num(5, 2)), int(num(8, 2))};
  *date_only = t.size() == 10;
  if (!*date_only) {
    if ((t[10] != 'T' && t[10] != ' ') || t.size() < 16 || t[13] != ':') return std::nullopt;
    c.h = int(num(11, 2)), c.mi = int(num(14, 2));
    size_t end = 16;
    if (t.size() >= 19 && t[16] == ':') c.s = int(num(17, 2)), end = 19;
    // fractions are dropped
    if (end < t.size() && t[end] == '.') {
      ++end;
      while (end < t.size() && t[end] >= '0' && t[end] <= '9') ++end;
    }
    if (end != t.size()) return std::nullopt;
  }
  if (!valid(c)) return std::nullopt;
  return c;
}

Civil Zone::to_local(int64_t utc_ms) const {
  int64_t s = utc_ms >= 0 ? utc_ms / 1000 : -((-utc_ms + 999) / 1000);
  return civil_from_seconds(s + at(utc_ms).offset_s);
}

int64_t Zone::to_utc(const Civil& local, int* adjusted) const {
  // Offsets a day either side: a zone changes at most once in so short a span.
  int64_t l = civil_seconds(local);
  int32_t before = at((l - 86400) * 1000).offset_s, after = at((l + 86400) * 1000).offset_s;
  std::vector<int64_t> fits;
  for (int32_t o : {before, after}) {
    int64_t u = l - o;
    if (at(u * 1000).offset_s == o && std::find(fits.begin(), fits.end(), u) == fits.end()) fits.push_back(u);
  }
  if (adjusted) *adjusted = 0;
  if (fits.empty()) {
    // A gap: the wall-clock time doesn't exist; it lands as far past the change as it was in.
    if (adjusted) *adjusted = 1;
    return (l - before) * 1000;
  }
  if (fits.size() > 1 && adjusted) *adjusted = 2;
  return *std::min_element(fits.begin(), fits.end()) * 1000;
}

std::optional<int64_t> Zone::next_change(int64_t) const { return std::nullopt; }

namespace {

class FixedZone : public Zone {
 public:
  explicit FixedZone(int32_t o) : offset_(o) {}
  std::string name() const override { return offset_ == 0 ? "UTC" : offset_text(offset_); }
  Info at(int64_t) const override { return {offset_, false, offset_ == 0 ? "UTC" : offset_text(offset_)}; }

 private:
  int32_t offset_;
};

class IanaZone : public Zone {
 public:
  explicit IanaZone(const ch::time_zone* z) : z_(z) {}
  std::string name() const override { return std::string(z_->name()); }
  Info at(int64_t utc_ms) const override {
    auto info = z_->get_info(ch::sys_time<ch::milliseconds>(ch::milliseconds(utc_ms)));
    return {int32_t(info.offset.count()), info.save != ch::minutes(0), info.abbrev};
  }
  int64_t to_utc(const Civil& local, int* adjusted) const override {
    auto ls = ch::local_seconds(ch::seconds(civil_seconds(local)));
    auto info = z_->get_info(ls);
    if (adjusted) *adjusted = info.result == ch::local_info::unique ? 0 : info.result == ch::local_info::nonexistent ? 1 : 2;
    // Unique: its offset. A gap: the offset before it, which lands after the change. An
    // overlap: the first (earlier) instant.
    return (civil_seconds(local) - info.first.offset.count()) * 1000;
  }
  std::optional<int64_t> next_change(int64_t utc_ms) const override {
    auto info = z_->get_info(ch::sys_time<ch::milliseconds>(ch::milliseconds(utc_ms)));
    if (info.end >= ch::sys_seconds::max() - ch::hours(24 * 366)) return std::nullopt;
    return ch::duration_cast<ch::milliseconds>(info.end.time_since_epoch()).count();
  }

 private:
  const ch::time_zone* z_;
};

class RulesZone : public Zone {
 public:
  RulesZone(std::string tzid, std::vector<Observance> obs) : tzid_(std::move(tzid)), obs_(std::move(obs)) {}
  std::string name() const override { return tzid_; }
  Info at(int64_t utc_ms) const override {
    // The latest onset at or before the instant, across every observance.
    int64_t best = INT64_MIN;
    const Observance* found = nullptr;
    Civil when = civil_from_seconds(utc_ms / 1000);
    for (const auto& o : obs_) {
      for (int64_t onset : onsets(o, when.y)) {
        if (onset <= utc_ms && onset > best) best = onset, found = &o;
      }
    }
    if (!found) {
      // Before every onset: the earliest observance's offset before it.
      const Observance* first = nullptr;
      for (const auto& o : obs_) {
        if (!first || o.start < first->start) first = &o;
      }
      if (!first) return {0, false, "UTC"};
      return {first->offset_from, false, first->name};
    }
    return {found->offset_to, found->daylight, found->name};
  }

 private:
  // Onsets (as UTC ms) of one observance, up to the year after `year`.
  std::vector<int64_t> onsets(const Observance& o, int64_t year) const {
    std::vector<int64_t> out;
    auto utc_of = [&](const Civil& c) { return (civil_seconds(c) - o.offset_from) * 1000; };
    out.push_back(utc_of(o.start));
    for (const auto& r : o.rdates) out.push_back(utc_of(r));
    if (!o.rrule.empty()) {
      std::string err;
      if (auto rule = parse_rule(o.rrule, &err)) {
        Civil horizon{year + 1, 12, 31, 23, 59, 59};
        auto e = expand(o.start, *rule, horizon, 2000, [&](const Civil& c) { return utc_of(c); });
        for (const auto& c : e.instances) out.push_back(utc_of(c));
      }
    }
    return out;
  }

  std::string tzid_;
  std::vector<Observance> obs_;
};

}  // namespace

std::shared_ptr<const Zone> utc() {
  static auto z = std::make_shared<FixedZone>(0);
  return z;
}

std::shared_ptr<const Zone> fixed(int32_t offset_s) { return std::make_shared<FixedZone>(offset_s); }

std::shared_ptr<const Zone> iana(std::string_view name) {
  if (name.empty() || name.size() > 64) return nullptr;
  if (name == "UTC" || name == "Etc/UTC" || name == "Z") return utc();
  static std::mutex mu;
  static std::map<std::string, std::shared_ptr<const Zone>, std::less<>> cache;
  std::lock_guard lock(mu);
  if (auto it = cache.find(name); it != cache.end()) return it->second;
  try {
    auto z = std::make_shared<IanaZone>(ch::locate_zone(name));
    cache.emplace(std::string(name), z);
    return z;
  } catch (...) {
    return nullptr;
  }
}

std::string tzdata_version() {
  try {
    return ch::get_tzdb().version;
  } catch (...) {
    return "unknown";
  }
}

std::vector<std::string> zone_names() {
  std::vector<std::string> out;
  try {
    for (const auto& z : ch::get_tzdb().zones) out.emplace_back(z.name());
    for (const auto& l : ch::get_tzdb().links) out.emplace_back(l.name());
  } catch (...) {
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::optional<std::string> windows_to_iana(std::string_view w) {
  // CLDR's windowsZones.xml, territory 001, for the zones calendars send most.
  static const std::pair<const char*, const char*> kMap[] = {
      {"Dateline Standard Time", "Etc/GMT+12"},
      {"Hawaiian Standard Time", "Pacific/Honolulu"},
      {"Alaskan Standard Time", "America/Anchorage"},
      {"Pacific Standard Time", "America/Los_Angeles"},
      {"Pacific Standard Time (Mexico)", "America/Tijuana"},
      {"US Mountain Standard Time", "America/Phoenix"},
      {"Mountain Standard Time", "America/Denver"},
      {"Central America Standard Time", "America/Guatemala"},
      {"Central Standard Time", "America/Chicago"},
      {"Central Standard Time (Mexico)", "America/Mexico_City"},
      {"Canada Central Standard Time", "America/Regina"},
      {"SA Pacific Standard Time", "America/Bogota"},
      {"Eastern Standard Time", "America/New_York"},
      {"US Eastern Standard Time", "America/Indianapolis"},
      {"Atlantic Standard Time", "America/Halifax"},
      {"Newfoundland Standard Time", "America/St_Johns"},
      {"E. South America Standard Time", "America/Sao_Paulo"},
      {"Argentina Standard Time", "America/Buenos_Aires"},
      {"UTC", "Etc/UTC"},
      {"GMT Standard Time", "Europe/London"},
      {"Greenwich Standard Time", "Atlantic/Reykjavik"},
      {"W. Europe Standard Time", "Europe/Berlin"},
      {"Central Europe Standard Time", "Europe/Budapest"},
      {"Romance Standard Time", "Europe/Paris"},
      {"Central European Standard Time", "Europe/Warsaw"},
      {"GTB Standard Time", "Europe/Bucharest"},
      {"FLE Standard Time", "Europe/Kiev"},
      {"E. Europe Standard Time", "Europe/Chisinau"},
      {"Israel Standard Time", "Asia/Jerusalem"},
      {"South Africa Standard Time", "Africa/Johannesburg"},
      {"Egypt Standard Time", "Africa/Cairo"},
      {"Turkey Standard Time", "Europe/Istanbul"},
      {"Russian Standard Time", "Europe/Moscow"},
      {"Arabian Standard Time", "Asia/Dubai"},
      {"Pakistan Standard Time", "Asia/Karachi"},
      {"India Standard Time", "Asia/Calcutta"},
      {"Nepal Standard Time", "Asia/Katmandu"},
      {"Bangladesh Standard Time", "Asia/Dhaka"},
      {"SE Asia Standard Time", "Asia/Bangkok"},
      {"China Standard Time", "Asia/Shanghai"},
      {"Singapore Standard Time", "Asia/Singapore"},
      {"Taipei Standard Time", "Asia/Taipei"},
      {"Tokyo Standard Time", "Asia/Tokyo"},
      {"Korea Standard Time", "Asia/Seoul"},
      {"AUS Eastern Standard Time", "Australia/Sydney"},
      {"E. Australia Standard Time", "Australia/Brisbane"},
      {"Cen. Australia Standard Time", "Australia/Adelaide"},
      {"W. Australia Standard Time", "Australia/Perth"},
      {"New Zealand Standard Time", "Pacific/Auckland"},
  };
  for (auto [win, zone] : kMap) {
    if (w == win) return std::string(zone);
  }
  return std::nullopt;
}

std::shared_ptr<const Zone> rules_zone(std::string tzid, std::vector<Observance> observances) {
  return std::make_shared<RulesZone>(std::move(tzid), std::move(observances));
}

std::optional<int32_t> parse_offset(std::string_view t) {
  if (t == "Z" || t == "z") return 0;
  if (t.size() != 5 && t.size() != 6 && t.size() != 7) return std::nullopt;
  if (t[0] != '+' && t[0] != '-') return std::nullopt;
  std::string digits;
  for (char c : t.substr(1)) {
    if (c == ':') continue;
    if (c < '0' || c > '9') return std::nullopt;
    digits += c;
  }
  if (digits.size() != 4 && digits.size() != 6) return std::nullopt;
  int h = std::stoi(digits.substr(0, 2)), m = std::stoi(digits.substr(2, 2));
  int s = digits.size() == 6 ? std::stoi(digits.substr(4, 2)) : 0;
  if (h > 18 || m > 59 || s > 59) return std::nullopt;
  int32_t v = h * 3600 + m * 60 + s;
  return t[0] == '-' ? -v : v;
}

std::string offset_text(int32_t o, bool colon) {
  char buf[16];
  int32_t a = o < 0 ? -o : o;
  std::snprintf(buf, sizeof buf, colon ? "%c%02d:%02d" : "%c%02d%02d", o < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
  return buf;
}

}  // namespace goober::calendar
