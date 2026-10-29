#include "goober/calendar/calendar.h"

#include <ctime>
#include <vector>

#include "goober/base/encoding.h"

namespace goober::calendar {

namespace {

bool digits(std::string_view s) {
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return !s.empty();
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}

std::string utc_stamp(int64_t unix_s) {
  std::time_t t = unix_s;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}

// "2026-11-05" (all day) or "2026-11-05T18:00[:00][.sss](Z|+hh:mm|-hh:mm)" as an iCalendar value.
std::optional<std::pair<std::string, bool>> when(std::string_view s) {
  if (s.size() == 10 && s[4] == '-' && s[7] == '-' && digits(s.substr(0, 4)) && digits(s.substr(5, 2)) && digits(s.substr(8, 2))) {
    return std::make_pair(std::string(s.substr(0, 4)) + std::string(s.substr(5, 2)) + std::string(s.substr(8, 2)), true);
  }
  if (s.size() < 17 || s[10] != 'T' || s[13] != ':') return std::nullopt;
  auto num = [&](size_t at, size_t n) { return digits(s.substr(at, n)) ? std::stoll(std::string(s.substr(at, n))) : -1LL; };
  int64_t y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2), sec = 0;
  size_t i = 16;
  if (i < s.size() && s[i] == ':') {
    sec = num(17, 2);
    i = 19;
  }
  while (i < s.size() && (s[i] == '.' || (s[i] >= '0' && s[i] <= '9'))) ++i;  // fractions
  if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60) {
    return std::nullopt;
  }
  int64_t offset_s = 0;
  std::string_view zone = s.substr(i);
  if (zone == "Z") {
    offset_s = 0;
  } else if (zone.size() == 6 && (zone[0] == '+' || zone[0] == '-') && zone[3] == ':' && digits(zone.substr(1, 2)) &&
             digits(zone.substr(4, 2))) {
    offset_s = (std::stoll(std::string(zone.substr(1, 2))) * 3600 + std::stoll(std::string(zone.substr(4, 2))) * 60) *
               (zone[0] == '-' ? -1 : 1);
  } else {
    return std::nullopt;  // a time needs its zone: Z or an offset
  }
  int64_t unix_s = ((days_from_civil(y, unsigned(mo), unsigned(d)) * 24 + h) * 60 + mi) * 60 + sec - offset_s;
  return std::make_pair(utc_stamp(unix_s), false);
}

// TEXT escaping (RFC 5545 § 3.3.11).
std::string text(std::string_view s) {
  std::string out;
  for (char c : base::valid_utf8(s)) {
    if (c == '\\' || c == ';' || c == ',') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c != '\r' && static_cast<unsigned char>(c) >= 0x20) {
      out += c;
    }
  }
  return out;
}

// A parameter value (CN=...): quoted, without the characters a quoted value can't hold.
std::string param(std::string_view s) {
  std::string out = "\"";
  for (char c : base::valid_utf8(s)) {
    if (c != '"' && static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out + "\"";
}

// Content lines at most 75 octets, folded with CRLF and a space, never inside a UTF-8 sequence.
std::string fold(const std::string& line) {
  std::string out;
  size_t start = 0, limit = 75;
  while (line.size() - start > limit) {
    size_t cut = start + limit;
    while (cut > start && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
    out += line.substr(start, cut - start) + "\r\n ";
    start = cut;
    limit = 74;  // the leading space counts
  }
  return out + line.substr(start) + "\r\n";
}

bool plain_address(std::string_view a) {
  auto at = a.find('@');
  return at != std::string_view::npos && at > 0 && at + 1 < a.size() && a.size() <= 254 &&
         a.find_first_of(" \t\r\n<>\"(),;:\\[]") == std::string_view::npos;
}

}  // namespace

std::optional<std::string> invite_ics(const Invite& inv, std::string_view domain, std::string_view organizer_email,
                                      std::string_view organizer_name, std::string_view attendee_email, int64_t now_ms,
                                      std::string* error) {
  auto bad = [&](std::string why) -> std::optional<std::string> {
    if (error) *error = std::move(why);
    return std::nullopt;
  };
  if (inv.uid.empty() || inv.uid.size() > 128 || inv.uid.find_first_of(" \t\r\n@;,\\:\"") != std::string::npos) {
    return bad("invite.uid is 1 to 128 characters without spaces, @, or punctuation, like meetup-7");
  }
  if (inv.summary.empty() || inv.summary.size() > 200) return bad("invite.summary is 1 to 200 characters");
  if (inv.description.size() > 8000 || inv.location.size() > 500) return bad("invite.description is at most 8000 bytes, location 500");
  if (!inv.url.empty() && !inv.url.starts_with("https://")) return bad("invite.url is an https URL");
  if (inv.sequence < 0 || inv.sequence > 100000) return bad("invite.sequence is 0 or more");
  if (!plain_address(organizer_email) || !plain_address(attendee_email)) return bad("the organizer and attendee are addresses");
  auto start = when(inv.start);
  auto end = inv.end.empty() ? std::nullopt : when(inv.end);
  if (!start) return bad("invite.start is a date (2026-11-05) or an RFC 3339 time with its zone (2026-11-05T18:00:00-06:00)");
  if (!inv.end.empty() && (!end || end->second != start->second)) return bad("invite.end is the same kind of value as start");
  if (end && end->first < start->first) return bad("invite.end is after start");
  std::string out;
  auto line = [&](std::string l) { out += fold(l); };
  line("BEGIN:VCALENDAR");
  line("PRODID:-//goober//goober calendar//EN");
  line("VERSION:2.0");
  line("CALSCALE:GREGORIAN");
  line(std::string("METHOD:") + (inv.cancel ? "CANCEL" : "REQUEST"));
  line("BEGIN:VEVENT");
  line("UID:" + inv.uid + "@" + std::string(domain));
  line("DTSTAMP:" + utc_stamp(now_ms / 1000));
  line("SEQUENCE:" + std::to_string(inv.sequence));
  std::string kind = start->second ? ";VALUE=DATE:" : ":";
  line("DTSTART" + kind + start->first);
  if (end) line("DTEND" + kind + end->first);
  line("SUMMARY:" + text(inv.summary));
  if (!inv.description.empty()) line("DESCRIPTION:" + text(inv.description));
  if (!inv.location.empty()) line("LOCATION:" + text(inv.location));
  if (!inv.url.empty()) line("URL:" + inv.url);
  line("ORGANIZER" + (organizer_name.empty() ? std::string() : ";CN=" + param(organizer_name)) + ":mailto:" +
       std::string(organizer_email));
  line("ATTENDEE;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:" + std::string(attendee_email));
  line(std::string("STATUS:") + (inv.cancel ? "CANCELLED" : "CONFIRMED"));
  line("END:VEVENT");
  line("END:VCALENDAR");
  return out;
}

std::optional<Reply> parse_reply(std::string_view ics) {
  // Unfold (RFC 5545 § 3.1): a line starting with a space or tab continues the one before.
  std::vector<std::string> lines;
  size_t i = 0;
  while (i < ics.size()) {
    size_t end = ics.find('\n', i);
    if (end == std::string_view::npos) end = ics.size();
    std::string l(ics.substr(i, end - i));
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty() && (l[0] == ' ' || l[0] == '\t') && !lines.empty()) {
      lines.back() += l.substr(1);
    } else {
      lines.push_back(std::move(l));
    }
    i = end + 1;
  }
  Reply r;
  bool in_event = false, any = false;
  for (const auto& l : lines) {
    auto colon = l.find(':');
    if (colon == std::string::npos) continue;
    std::string head = base::to_lower(l.substr(0, colon)), value = l.substr(colon + 1);
    std::string name = head.substr(0, head.find(';'));
    if (name == "begin" && base::to_lower(value) == "vevent") in_event = true;
    if (name == "end" && base::to_lower(value) == "vevent") in_event = false;
    if (name == "method") r.method = value, any = true;
    if (!in_event) continue;
    if (name == "uid") r.uid = value;
    if (name == "sequence" && digits(value)) r.sequence = std::stoll(value);
    if (name == "comment") r.comment = value;
    if (name == "attendee" && r.attendee.empty()) {
      std::string v = base::to_lower(value);
      r.attendee = v.starts_with("mailto:") ? v.substr(7) : v;
      // The PARTSTAT parameter, from the head as written (its case kept for the value).
      std::string upper = l.substr(0, colon);
      for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
      if (auto p = upper.find(";PARTSTAT="); p != std::string::npos) {
        size_t s = p + 10, e = upper.find(';', s);
        r.partstat = upper.substr(s, e == std::string::npos ? std::string::npos : e - s);
      }
    }
  }
  if (!any || r.uid.empty()) return std::nullopt;
  for (auto& c : r.method) c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

}  // namespace goober::calendar
