#include "goober/base/time.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace goober::base {

int64_t now_unix_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t monotonic_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string format_utc_ms(int64_t unix_ms) {
  time_t seconds = time_t(unix_ms / 1000);
  int millis = int(unix_ms % 1000);
  if (millis < 0) {
    millis += 1000;
    seconds -= 1;
  }
  std::tm tm{};
  gmtime_r(&seconds, &tm);
  char buf[96];  // room for any int in each field, so the compiler can see nothing is cut
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
  return buf;
}

}  // namespace goober::base
