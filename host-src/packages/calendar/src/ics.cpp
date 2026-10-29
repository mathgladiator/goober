#include "goober/calendar/ics.h"

#include <algorithm>

#include "goober/base/encoding.h"

namespace goober::calendar {

namespace {

constexpr size_t kMaxProps = 10000;
constexpr int kMaxDepth = 8;

std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

int64_t num(std::string_view s, size_t at, size_t n) {
  if (at + n > s.size()) return -1;
  int64_t v = 0;
  for (size_t i = at; i < at + n; ++i) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

// One content line into a Property: name *(";" param) ":" value, with quoted parameter values.
std::optional<Property> content_line(std::string_view line) {
  Property p;
  size_t i = 0;
  while (i < line.size() && line[i] != ';' && line[i] != ':') ++i;
  if (i == 0 || i == line.size()) return std::nullopt;
  p.name = upper(line.substr(0, i));
  while (i < line.size() && line[i] == ';') {
    ++i;
    size_t eq = line.find('=', i);
    if (eq == std::string_view::npos) return std::nullopt;
    Param param;
    param.name = upper(line.substr(i, eq - i));
    i = eq + 1;
    while (true) {
      std::string v;
      if (i < line.size() && line[i] == '"') {
        size_t close = line.find('"', i + 1);
        if (close == std::string_view::npos) return std::nullopt;
        v = std::string(line.substr(i + 1, close - i - 1));
        i = close + 1;
      } else {
        size_t e = i;
        while (e < line.size() && line[e] != ',' && line[e] != ';' && line[e] != ':') ++e;
        v = std::string(line.substr(i, e - i));
        i = e;
      }
      param.values.push_back(std::move(v));
      if (i < line.size() && line[i] == ',') {
        ++i;
        continue;
      }
      break;
    }
    p.params.push_back(std::move(param));
  }
  if (i >= line.size() || line[i] != ':') return std::nullopt;
  p.value = std::string(line.substr(i + 1));
  return p;
}

// A parameter value is quoted only when it holds what would end it (RFC 5545 § 3.1: spaces are
// safe as they are).
bool needs_quotes(std::string_view v) { return v.find_first_of(":;,") != std::string_view::npos; }

std::string fold(const std::string& line) {
  std::string out;
  size_t start = 0, limit = 75;
  while (line.size() - start > limit) {
    size_t cut = start + limit;
    while (cut > start + 1 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
    out += line.substr(start, cut - start) + "\r\n ";
    start = cut;
    limit = 74;
  }
  return out + line.substr(start) + "\r\n";
}

void write(const Component& c, std::string& out) {
  out += fold("BEGIN:" + c.name);
  for (const auto& p : c.props) {
    std::string line = p.name;
    for (const auto& prm : p.params) {
      line += ";" + prm.name + "=";
      for (size_t i = 0; i < prm.values.size(); ++i) {
        std::string v;
        for (char ch : prm.values[i]) {
          if (ch != '"' && ch != '\r' && ch != '\n') v += ch;
        }
        line += (i ? "," : "") + (needs_quotes(v) ? "\"" + v + "\"" : v);
      }
    }
    std::string value;
    for (char ch : p.value) {
      if (ch != '\r' && ch != '\n') value += ch;  // a value is one line; TEXT escapes its newlines
    }
    out += fold(line + ":" + value);
  }
  for (const auto& child : c.children) write(child, out);
  out += fold("END:" + c.name);
}

}  // namespace

const Param* Property::param(std::string_view n) const {
  for (const auto& p : params) {
    if (p.name == n) return &p;
  }
  return nullptr;
}

std::string Property::param_value(std::string_view n) const {
  auto* p = param(n);
  return p && !p->values.empty() ? p->values.front() : "";
}

const Property* Component::prop(std::string_view n) const {
  for (const auto& p : props) {
    if (p.name == n) return &p;
  }
  return nullptr;
}

std::vector<const Property*> Component::all(std::string_view n) const {
  std::vector<const Property*> out;
  for (const auto& p : props) {
    if (p.name == n) out.push_back(&p);
  }
  return out;
}

std::string Component::text(std::string_view n) const {
  auto* p = prop(n);
  return p ? unescape_text(p->value) : "";
}

std::optional<Parsed> parse(std::string_view text, std::string* error) {
  if (text.size() > kMaxIcsBytes) {
    if (error) *error = "a calendar is at most 1 MiB";
    return std::nullopt;
  }
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  // Unfold: a line starting with a space or tab continues the one before (RFC 5545 § 3.1).
  std::vector<std::string> lines;
  size_t i = 0;
  while (i < text.size()) {
    size_t end = text.find('\n', i);
    if (end == std::string_view::npos) end = text.size();
    std::string_view l = text.substr(i, end - i);
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    if (!l.empty() && (l[0] == ' ' || l[0] == '\t') && !lines.empty()) {
      lines.back() += std::string(l.substr(1));
    } else if (!l.empty()) {
      lines.emplace_back(l);
    }
    i = end + 1;
  }
  Parsed out;
  std::vector<Component> stack;
  bool limited = false;
  for (const auto& line : lines) {
    auto p = content_line(line);
    if (!p) {
      if (out.warnings.size() < 50) out.warnings.push_back("not a content line: " + line.substr(0, 80));
      continue;
    }
    if (p->name == "BEGIN") {
      if (int(stack.size()) >= kMaxDepth) {
        limited = true;
        continue;
      }
      stack.push_back(Component{upper(p->value), {}, {}});
      continue;
    }
    if (p->name == "END") {
      std::string name = upper(p->value);
      if (stack.empty()) continue;
      // An END for an outer component closes what's open inside it too.
      auto open = std::find_if(stack.rbegin(), stack.rend(), [&](const Component& c) { return c.name == name; });
      if (open == stack.rend()) {
        if (out.warnings.size() < 50) out.warnings.push_back("END:" + name + " closes nothing open");
        continue;
      }
      while (!stack.empty()) {
        Component c = std::move(stack.back());
        stack.pop_back();
        bool last = c.name == name;
        if (stack.empty()) {
          out.components.push_back(std::move(c));
        } else {
          stack.back().children.push_back(std::move(c));
        }
        if (last) break;
      }
      continue;
    }
    if (stack.empty()) continue;  // a property outside any component
    if (stack.back().props.size() >= kMaxProps) {
      limited = true;
      continue;
    }
    stack.back().props.push_back(std::move(*p));
  }
  while (!stack.empty()) {
    if (out.warnings.size() < 50) out.warnings.push_back(stack.back().name + " was never closed");
    Component c = std::move(stack.back());
    stack.pop_back();
    if (stack.empty()) {
      out.components.push_back(std::move(c));
    } else {
      stack.back().children.push_back(std::move(c));
    }
  }
  if (limited) out.warnings.push_back("past the limits (8 deep, 10000 properties a component): the rest was dropped");
  if (out.components.empty()) {
    if (error) *error = "no BEGIN:VCALENDAR (or any component) in it";
    return std::nullopt;
  }
  return out;
}

std::string serialize(const Component& c) {
  std::string out;
  write(c, out);
  return out;
}

std::string unescape_text(std::string_view v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char n = v[++i];
      out += n == 'n' || n == 'N' ? '\n' : n;
    } else {
      out += v[i];
    }
  }
  return out;
}

std::string escape_text(std::string_view v) {
  std::string out;
  for (char c : base::valid_utf8(v)) {
    if (c == '\\' || c == ';' || c == ',') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c != '\r' && static_cast<unsigned char>(c) >= 0x20) {
      out += c;
    } else if (c == '\t') {
      out += ' ';
    }
  }
  return out;
}

Zones calendar_zones(const Component& cal, std::vector<std::string>* warnings) {
  Zones out;
  for (const auto& tz : cal.children) {
    if (tz.name != "VTIMEZONE") continue;
    std::string tzid = tz.text("TZID");
    if (tzid.empty()) continue;
    std::vector<Observance> obs;
    for (const auto& o : tz.children) {
      if (o.name != "STANDARD" && o.name != "DAYLIGHT") continue;
      Observance ob;
      ob.daylight = o.name == "DAYLIGHT";
      auto* s = o.prop("DTSTART");
      auto from = o.prop("TZOFFSETFROM") ? parse_offset(o.prop("TZOFFSETFROM")->value) : std::nullopt;
      auto to = o.prop("TZOFFSETTO") ? parse_offset(o.prop("TZOFFSETTO")->value) : std::nullopt;
      if (!s || !from || !to) continue;
      std::string v = s->value;
      if (!v.empty() && v.back() == 'Z') v.pop_back();
      Civil c{num(v, 0, 4), int(num(v, 4, 2)), int(num(v, 6, 2))};
      if (v.size() >= 15) c.h = int(num(v, 9, 2)), c.mi = int(num(v, 11, 2)), c.s = int(num(v, 13, 2));
      if (!valid(c)) continue;
      ob.start = c;
      ob.offset_from = *from;
      ob.offset_to = *to;
      if (auto* r = o.prop("RRULE")) ob.rrule = r->value;
      for (auto* rd : o.all("RDATE")) {
        std::string d = rd->value;
        if (d.size() >= 15) {
          Civil rc{num(d, 0, 4), int(num(d, 4, 2)), int(num(d, 6, 2)), int(num(d, 9, 2)), int(num(d, 11, 2)), int(num(d, 13, 2))};
          if (valid(rc)) ob.rdates.push_back(rc);
        }
      }
      ob.name = o.text("TZNAME");
      obs.push_back(std::move(ob));
    }
    if (obs.empty()) {
      if (warnings) warnings->push_back("VTIMEZONE " + tzid + " has no usable observances");
      continue;
    }
    out[tzid] = rules_zone(tzid, std::move(obs));
  }
  return out;
}

std::optional<When> parse_when(std::string_view value, std::string_view tzid_in, bool date_only, const Zones& zones,
                               std::string_view fallback, std::vector<std::string>* warnings) {
  std::string v(value);
  bool z = !v.empty() && (v.back() == 'Z' || v.back() == 'z');
  if (z) v.pop_back();
  When w;
  if (v.size() == 8 || date_only) {
    if (v.size() < 8) return std::nullopt;
    w.date_only = true;
    w.local = Civil{num(v, 0, 4), int(num(v, 4, 2)), int(num(v, 6, 2))};
  } else {
    if (v.size() != 15 || (v[8] != 'T' && v[8] != 't')) return std::nullopt;
    w.local = Civil{num(v, 0, 4), int(num(v, 4, 2)), int(num(v, 6, 2)), int(num(v, 9, 2)), int(num(v, 11, 2)),
                    int(num(v, 13, 2))};
  }
  if (!valid(w.local)) return std::nullopt;
  std::shared_ptr<const Zone> zone;
  std::string tzid(tzid_in);
  // Some writers send "/America/Chicago" or a quoted name.
  while (!tzid.empty() && (tzid.front() == '/' || tzid.front() == '"')) tzid.erase(tzid.begin());
  while (!tzid.empty() && tzid.back() == '"') tzid.pop_back();
  if (z) {
    zone = utc();
    w.zone = "UTC";
  } else if (!tzid.empty()) {
    // The calendar's own definition first (RFC 5545 § 3.2.19), then IANA, then Windows' names.
    if (auto it = zones.find(std::string(tzid_in)); it != zones.end()) {
      zone = it->second;
      // Named by what others can resolve: its IANA name, or Windows' name mapped to one; a
      // zone known only by its own definition is given as instants (UTC) from here on.
      if (iana(tzid)) {
        w.zone = tzid;
      } else if (auto mapped = windows_to_iana(tzid); mapped && iana(*mapped)) {
        w.zone = *mapped;
      } else {
        w.zone = "UTC";
      }
    } else if (auto ia = iana(tzid)) {
      zone = ia;
      w.zone = tzid;
    } else if (auto mapped = windows_to_iana(tzid); mapped && iana(*mapped)) {
      zone = iana(*mapped);
      w.zone = *mapped;
    } else {
      if (warnings) warnings->push_back("unknown TZID " + tzid + ": read as " + (fallback.empty() ? "UTC" : std::string(fallback)));
    }
  }
  if (!zone) {
    if (!fallback.empty()) zone = iana(fallback);
    if (!zone) {
      zone = utc();
      if (tzid.empty() && !w.date_only && warnings) warnings->push_back("a floating time, read as UTC");
    }
    if (w.zone.empty() && !w.date_only) w.zone = tzid.empty() ? "" : zone->name();
  }
  int adjusted = 0;
  w.ms = zone->to_utc(w.local, &adjusted);
  return w;
}

std::optional<When> when_of(const Property& p, const Zones& zones, std::string_view fallback,
                            std::vector<std::string>* warnings) {
  bool date_only = upper(p.param_value("VALUE")) == "DATE";
  return parse_when(p.value, p.param_value("TZID"), date_only, zones, fallback, warnings);
}

std::string when_value(const When& w) {
  char buf[32];
  const Civil& c = w.local;
  if (w.date_only) {
    std::snprintf(buf, sizeof buf, "%04lld%02d%02d", (long long)c.y, c.mo, c.d);
    return buf;
  }
  if (w.zone == "UTC" || w.zone.empty()) {
    Civil u = civil_from_seconds(w.ms / 1000);
    std::snprintf(buf, sizeof buf, "%04lld%02d%02dT%02d%02d%02dZ", (long long)u.y, u.mo, u.d, u.h, u.mi, u.s);
    return buf;
  }
  std::snprintf(buf, sizeof buf, "%04lld%02d%02dT%02d%02d%02d", (long long)c.y, c.mo, c.d, c.h, c.mi, c.s);
  return buf;
}

std::optional<int64_t> parse_duration(std::string_view v) {
  bool neg = false;
  if (!v.empty() && (v[0] == '+' || v[0] == '-')) {
    neg = v[0] == '-';
    v.remove_prefix(1);
  }
  if (v.empty() || (v[0] != 'P' && v[0] != 'p')) return std::nullopt;
  v.remove_prefix(1);
  int64_t total = 0, n = 0;
  bool time = false, any = false, digits = false;
  for (char c : v) {
    char u = char(std::toupper(static_cast<unsigned char>(c)));
    if (u >= '0' && u <= '9') {
      n = n * 10 + (u - '0');
      digits = true;
      if (n > 100000000) return std::nullopt;
      continue;
    }
    if (u == 'T') {
      time = true;
      continue;
    }
    if (!digits) return std::nullopt;
    int64_t unit = 0;
    if (u == 'W' && !time) unit = 7 * 86400;
    else if (u == 'D' && !time) unit = 86400;
    else if (u == 'H' && time) unit = 3600;
    else if (u == 'M' && time) unit = 60;
    else if (u == 'S' && time) unit = 1;
    else return std::nullopt;
    total += n * unit;
    n = 0;
    digits = false;
    any = true;
  }
  if (!any || digits) return std::nullopt;
  return neg ? -total : total;
}

std::string duration_text(int64_t s) {
  std::string out = s < 0 ? "-P" : "P";
  if (s < 0) s = -s;
  if (s == 0) return "PT0S";
  int64_t d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60, sec = s % 60;
  if (d && d % 7 == 0 && !h && !m && !sec) return out + std::to_string(d / 7) + "W";
  if (d) out += std::to_string(d) + "D";
  if (h || m || sec) {
    out += "T";
    if (h) out += std::to_string(h) + "H";
    if (m) out += std::to_string(m) + "M";
    if (sec) out += std::to_string(sec) + "S";
  }
  return out;
}

}  // namespace goober::calendar
