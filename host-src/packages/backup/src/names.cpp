// Stamps and restore points.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>

#include "goober/backup/backup.h"

namespace goober::backup {

std::string stamp(int64_t unix_ms) {
  std::time_t secs = unix_ms / 1000;
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H-%M-%SZ", &tm);
  return buf;
}

namespace {

std::optional<int64_t> utc_ms(int y, int mo, int d, int h, int mi, int s) {
  using namespace std::chrono;
  year_month_day ymd{year(y), month(unsigned(mo)), day(unsigned(d))};
  if (!ymd.ok() || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) return std::nullopt;
  return duration_cast<milliseconds>((sys_days(ymd) + hours(h) + minutes(mi) + seconds(s)).time_since_epoch()).count();
}

}  // namespace

std::optional<int64_t> parse_stamp(std::string_view text) {
  int y, mo, d, h, mi, s;
  char z;
  if (text.size() != 20) return std::nullopt;
  if (std::sscanf(std::string(text).c_str(), "%4d-%2d-%2dT%2d-%2d-%2d%c", &y, &mo, &d, &h, &mi, &s, &z) != 7 || z != 'Z') {
    return std::nullopt;
  }
  return utc_ms(y, mo, d, h, mi, s);
}

std::optional<int64_t> parse_moment(std::string_view text) {
  if (auto s = parse_stamp(text)) return s;
  std::string t(text);
  if (!t.empty() && t.back() == 'Z') t.pop_back();
  int y = 0, mo = 0, d = 0, h = 23, mi = 59, s = 59;
  int n = 0;
  if (t.size() == 10 && std::sscanf(t.c_str(), "%4d-%2d-%2d%n", &y, &mo, &d, &n) == 3 && n == 10) {
    return utc_ms(y, mo, d, h, mi, s);  // a day means its end
  }
  s = 0;
  if (t.size() == 16 && std::sscanf(t.c_str(), "%4d-%2d-%2dT%2d:%2d%n", &y, &mo, &d, &h, &mi, &n) == 5 && n == 16) {
    return utc_ms(y, mo, d, h, mi, s);
  }
  if (t.size() == 19 && std::sscanf(t.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &n) == 6 && n == 19) {
    return utc_ms(y, mo, d, h, mi, s);
  }
  return std::nullopt;
}

std::vector<RestorePoint> restore_points(const std::vector<std::string>& keys) {
  std::map<std::string, bool> nights;
  std::vector<RestorePoint> out;
  for (const auto& k : keys) {
    if (k.starts_with("nightly/") && k.ends_with(".tar.gz.sealed")) {
      std::string s = k.substr(8, k.size() - 8 - 14);
      if (auto at = parse_stamp(s)) {
        nights[s] = true;
        out.push_back(RestorePoint{s, "", *at});
      }
    }
  }
  for (const auto& k : keys) {
    // increments/NIGHT/STAMP/MANIFEST.json.sealed, for a night that is still there.
    if (!k.starts_with("increments/") || !k.ends_with("/MANIFEST.json.sealed")) continue;
    std::string rest = k.substr(11);
    auto slash = rest.find('/');
    if (slash == std::string::npos) continue;
    std::string night = rest.substr(0, slash);
    std::string at = rest.substr(slash + 1, rest.size() - slash - 1 - 21);
    auto at_ms = parse_stamp(at);
    if (!nights.count(night) || !at_ms) continue;
    out.push_back(RestorePoint{night, at, *at_ms});
  }
  std::sort(out.begin(), out.end(), [](const RestorePoint& a, const RestorePoint& b) {
    return a.at_ms != b.at_ms ? a.at_ms < b.at_ms : a.increment < b.increment;
  });
  return out;
}

std::optional<RestorePoint> choose_point(const std::vector<RestorePoint>& points, int64_t at_ms) {
  std::optional<RestorePoint> best;
  for (const auto& p : points) {
    if (at_ms == 0 || p.at_ms <= at_ms) best = p;
  }
  return best;
}

std::string source_revision_name(int64_t rev) {
  std::string n = std::to_string(rev);
  return n.size() >= 6 ? n : std::string(6 - n.size(), '0') + n;
}

}  // namespace goober::backup
