// ics@1 (design/machine/831.Libraries.md § 5): iCalendar as events in JSON, and iTIP (RFC 5546):
// invites, replies, proposals (COUNTER) and their answers, cancellations, and expansion.

#include <algorithm>
#include <set>

#include "common.h"
#include "goober/base/encoding.h"
#include "goober/calendar/ics.h"
#include "goober/calendar/rrule.h"

namespace goober::libraries {

using namespace detail;
namespace cal = calendar;

namespace {

constexpr const char* kProdId = "-//goober//ics@1//EN";
constexpr int64_t kMaxInstances = 10000;

std::string upper(std::string s) {
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string address_of(std::string_view v) {
  std::string s = base::to_lower(v);
  if (s.starts_with("mailto:")) s = s.substr(7);
  return s;
}

bool plain_address(std::string_view a) {
  auto at = a.find('@');
  return at != std::string_view::npos && at > 0 && at + 1 < a.size() && a.size() <= 254 &&
         a.find_first_of(" \t\r\n<>\"(),;:\\[]") == std::string_view::npos;
}

// ---- values to and from JSON

bj::object when_json(const cal::When& w) {
  if (w.date_only) return bj::object{{"date", cal::civil_text(w.local, true)}};
  bj::object o{{"ms", w.ms}, {"utc", utc_text(w.ms)}};
  if (!w.zone.empty() && w.zone != "UTC") {
    o["local"] = cal::civil_text(w.local);
    o["zone"] = w.zone;
  }
  return o;
}

// {date} | {ms} | {utc} | {local, zone} | RFC 3339 text.
cal::When when_from(const bj::value& v, Context& ctx, const std::string& field) {
  cal::When w;
  if (v.is_object()) {
    const auto& o = v.as_object();
    if (auto* d = o.if_contains("date"); d && !o.contains("time")) {
      bool date_only = false;
      auto c = d->is_string() ? cal::parse_civil_text(d->as_string(), &date_only) : std::nullopt;
      if (!c || !date_only) bad(field + ".date is like 2026-11-07");
      w.date_only = true;
      w.local = *c;
      w.ms = cal::civil_seconds(*c) * 1000;
      return w;
    }
    if (auto* l = o.if_contains("local")) {
      bool date_only = false;
      auto c = local_of(*l, &date_only);
      if (!c || date_only) bad(field + ".local is like 2026-11-05T18:00:00");
      std::string zn = text(o, "zone");
      if (zn.empty()) bad(field + " with local needs zone");
      auto z = zone_of(zn, ctx);
      w.local = *c;
      w.zone = z->name();
      w.ms = z->to_utc(*c, nullptr);
      return w;
    }
    for (const char* k : {"ms", "utc"}) {
      if (auto* x = o.if_contains(k)) {
        auto ms = instant(*x);
        if (!ms) bad(field + "." + k + " is an instant");
        w.ms = *ms;
        w.zone = "UTC";
        w.local = cal::civil_from_seconds(*ms / 1000);
        return w;
      }
    }
  }
  if (auto ms = instant(v)) {
    w.ms = *ms;
    w.zone = "UTC";
    w.local = cal::civil_from_seconds(*ms / 1000);
    return w;
  }
  // A date alone, as text: an all-day value.
  if (v.is_string()) {
    bool date_only = false;
    auto c = cal::parse_civil_text(v.as_string(), &date_only);
    if (c && date_only) {
      w.date_only = true;
      w.local = *c;
      w.ms = cal::civil_seconds(*c) * 1000;
      return w;
    }
  }
  bad(field + " is {date}, {ms}, {utc}, {local, zone}, RFC 3339 text, or a date (2026-11-07)");
}

cal::Property when_prop(const std::string& name, const cal::When& w) {
  cal::Property p{name, {}, cal::when_value(w)};
  if (w.date_only) {
    p.params.push_back({"VALUE", {"DATE"}});
  } else if (!w.zone.empty() && w.zone != "UTC") {
    p.params.push_back({"TZID", {w.zone}});
  }
  return p;
}

bj::object person_json(const cal::Property& p) {
  bj::object o{{"email", address_of(p.value)}};
  if (auto cn = p.param_value("CN"); !cn.empty()) o["name"] = cn;
  if (p.name == "ATTENDEE") {
    std::string ps = upper(p.param_value("PARTSTAT"));
    o["partstat"] = ps.empty() ? "NEEDS-ACTION" : ps;
    std::string role = upper(p.param_value("ROLE"));
    o["role"] = role.empty() ? "REQ-PARTICIPANT" : role;
    o["rsvp"] = upper(p.param_value("RSVP")) == "TRUE";
    if (auto d = p.param_value("DELEGATED-TO"); !d.empty()) o["delegated_to"] = address_of(d);
  }
  return o;
}

cal::Property person_prop(const std::string& name, const bj::object& o, const std::string& field) {
  std::string email = base::to_lower(text(o, "email", true));
  if (email.starts_with("mailto:")) email = email.substr(7);
  if (!plain_address(email)) bad(field + ".email is one address");
  cal::Property p{name, {}, "mailto:" + email};
  if (auto n = text(o, "name"); !n.empty()) p.params.push_back({"CN", {n.substr(0, 200)}});
  if (name == "ATTENDEE") {
    std::string role = upper(text(o, "role"));
    p.params.push_back({"ROLE", {role.empty() ? "REQ-PARTICIPANT" : role}});
    std::string ps = upper(text(o, "partstat"));
    static const std::set<std::string> kPart = {"NEEDS-ACTION", "ACCEPTED", "DECLINED", "TENTATIVE", "DELEGATED"};
    if (!ps.empty() && !kPart.count(ps)) bad(field + ".partstat is NEEDS-ACTION, ACCEPTED, DECLINED, TENTATIVE, or DELEGATED");
    p.params.push_back({"PARTSTAT", {ps.empty() ? "NEEDS-ACTION" : ps}});
    auto* rsvp = o.if_contains("rsvp");
    if (!rsvp || (rsvp->is_bool() && rsvp->as_bool())) p.params.push_back({"RSVP", {"TRUE"}});
  }
  return p;
}

struct Cal {
  std::string method, prodid;
  std::vector<const cal::Component*> events, todos;
  cal::Zones zones;
  std::string fallback;
};

// The calendars in a parse, read once for their zones and events.
std::vector<Cal> calendars(const cal::Parsed& parsed, std::vector<std::string>* warnings) {
  std::vector<Cal> out;
  for (const auto& top : parsed.components) {
    Cal c;
    if (top.name == "VCALENDAR") {
      c.method = upper(top.text("METHOD"));
      c.prodid = top.text("PRODID");
      c.zones = cal::calendar_zones(top, warnings);
      c.fallback = top.text("X-WR-TIMEZONE");
      if (!c.fallback.empty() && !cal::iana(c.fallback)) c.fallback.clear();
      for (const auto& ch : top.children) {
        if (ch.name == "VEVENT") c.events.push_back(&ch);
        if (ch.name == "VTODO") c.todos.push_back(&ch);
      }
    } else if (top.name == "VEVENT") {
      c.events.push_back(&top);
    }
    out.push_back(std::move(c));
  }
  return out;
}

bj::object event_json(const cal::Component& e, const Cal& c, std::vector<std::string>* warnings) {
  bj::object o;
  o["uid"] = e.text("UID");
  int64_t seq = 0;
  if (auto* s = e.prop("SEQUENCE")) {
    try {
      seq = std::stoll(s->value);
    } catch (...) {
    }
  }
  o["sequence"] = seq;
  for (auto [k, prop] : {std::pair{"status", "STATUS"}, std::pair{"summary", "SUMMARY"}, std::pair{"description", "DESCRIPTION"},
                         std::pair{"location", "LOCATION"}, std::pair{"url", "URL"}, std::pair{"comment", "COMMENT"},
                         std::pair{"transp", "TRANSP"}, std::pair{"class", "CLASS"}}) {
    if (e.prop(prop)) o[k] = std::string(k) == "url" ? e.prop(prop)->value : e.text(prop);
  }
  std::optional<cal::When> start, end;
  if (auto* p = e.prop("DTSTART")) start = cal::when_of(*p, c.zones, c.fallback, warnings);
  if (auto* p = e.prop("DTEND")) end = cal::when_of(*p, c.zones, c.fallback, warnings);
  if (!end && start) {
    if (auto* d = e.prop("DURATION")) {
      if (auto secs = cal::parse_duration(d->value)) {
        cal::When w = *start;
        w.ms += *secs * 1000;
        w.local = cal::civil_from_seconds(cal::civil_seconds(w.local) + *secs);
        end = w;
      }
    } else if (start->date_only) {  // an all-day event without an end lasts the day
      cal::When w = *start;
      w.local = cal::civil_from_seconds(cal::civil_seconds(w.local) + 86400);
      w.ms += 86400000;
      end = w;
    }
  }
  if (start) o["start"] = when_json(*start);
  if (end) o["end"] = when_json(*end);
  o["all_day"] = start && start->date_only;
  if (auto* r = e.prop("RRULE")) o["rrule"] = r->value;
  for (auto [k, prop] : {std::pair{"exdates", "EXDATE"}, std::pair{"rdates", "RDATE"}}) {
    bj::array list;
    for (auto* p : e.all(prop)) {
      // A list property: comma-separated values sharing the parameters.
      size_t i = 0;
      while (i <= p->value.size()) {
        size_t comma = p->value.find(',', i);
        if (comma == std::string::npos) comma = p->value.size();
        cal::Property one{p->name, p->params, p->value.substr(i, comma - i)};
        if (auto w = cal::when_of(one, c.zones, c.fallback, warnings)) list.push_back(when_json(*w));
        i = comma + 1;
      }
    }
    if (!list.empty()) o[k] = std::move(list);
  }
  if (auto* rid = e.prop("RECURRENCE-ID")) {
    if (auto w = cal::when_of(*rid, c.zones, c.fallback, warnings)) o["recurrence_id"] = when_json(*w);
  }
  if (auto* org = e.prop("ORGANIZER")) o["organizer"] = person_json(*org);
  bj::array attendees;
  for (auto* a : e.all("ATTENDEE")) attendees.push_back(person_json(*a));
  o["attendees"] = std::move(attendees);
  bj::array categories;
  for (auto* p : e.all("CATEGORIES")) {
    size_t i = 0;
    while (i <= p->value.size()) {
      size_t comma = p->value.find(',', i);
      while (comma != std::string::npos && comma > 0 && p->value[comma - 1] == '\\') comma = p->value.find(',', comma + 1);
      if (comma == std::string::npos) comma = p->value.size();
      std::string v = cal::unescape_text(p->value.substr(i, comma - i));
      if (!v.empty()) categories.push_back(bj::value(v));
      i = comma + 1;
    }
  }
  if (!categories.empty()) o["categories"] = std::move(categories);
  bj::array alarms;
  for (const auto& a : e.children) {
    if (a.name != "VALARM") continue;
    bj::object al{{"action", upper(a.text("ACTION"))}};
    if (auto* t = a.prop("TRIGGER")) al["trigger"] = t->value;
    if (a.prop("DESCRIPTION")) al["description"] = a.text("DESCRIPTION");
    alarms.push_back(std::move(al));
  }
  if (!alarms.empty()) o["alarms"] = std::move(alarms);
  if (auto* st = e.prop("DTSTAMP")) {
    if (auto w = cal::when_of(*st, c.zones, "", nullptr)) o["dtstamp"] = utc_text(w->ms);
  }
  // Everything else, as written, so nothing is lost in a round trip.
  static const std::set<std::string> kKnown = {"UID", "SEQUENCE", "STATUS", "SUMMARY", "DESCRIPTION", "LOCATION", "URL",
                                               "COMMENT", "TRANSP", "CLASS", "DTSTART", "DTEND", "DURATION", "RRULE",
                                               "EXDATE", "RDATE", "RECURRENCE-ID", "ORGANIZER", "ATTENDEE", "CATEGORIES",
                                               "DTSTAMP"};
  bj::object other;
  for (const auto& p : e.props) {
    if (!kKnown.count(p.name) && other.size() < 50 && !other.contains(p.name)) other[p.name] = p.value.substr(0, 2000);
  }
  if (!other.empty()) o["other"] = std::move(other);
  return o;
}

std::string limited(const bj::object& o, const char* k, size_t max, const std::string& field) {
  std::string v = text(o, k);
  if (v.size() > max) bad(field + "." + k + " is at most " + std::to_string(max) + " bytes");
  return v;
}

cal::Component event_component(const bj::object& e, Context& ctx, const std::string& field) {
  cal::Component c{"VEVENT", {}, {}};
  std::string uid = text(e, "uid", true);
  if (uid.empty() || uid.size() > 255 || uid.find_first_of("\r\n") != std::string::npos) bad(field + ".uid is 1 to 255 characters");
  c.props.push_back({"UID", {}, cal::escape_text(uid)});
  c.props.push_back({"DTSTAMP", {}, cal::when_value(cal::When{false, cal::civil_from_seconds(ctx.now_ms / 1000), "UTC", ctx.now_ms})});
  int64_t seq = integer(e, "sequence").value_or(0);
  if (seq < 0 || seq > 1000000) bad(field + ".sequence is 0 or more");
  c.props.push_back({"SEQUENCE", {}, std::to_string(seq)});
  auto* s = e.if_contains("start");
  if (!s) bad(field + ".start is required");
  auto start = when_from(*s, ctx, field + ".start");
  c.props.push_back(when_prop("DTSTART", start));
  if (auto* en = e.if_contains("end"); en && !en->is_null()) {
    auto end = when_from(*en, ctx, field + ".end");
    if (end.date_only != start.date_only) bad(field + ".end is the same kind as start (both dates, or both times)");
    if (end.ms < start.ms) bad(field + ".end is after start");
    c.props.push_back(when_prop("DTEND", end));
  }
  for (auto [k, prop, max] : {std::tuple{"summary", "SUMMARY", 1000}, std::tuple{"description", "DESCRIPTION", 16000},
                              std::tuple{"location", "LOCATION", 1000}, std::tuple{"comment", "COMMENT", 2000}}) {
    std::string v = limited(e, k, size_t(max), field);
    if (!v.empty()) c.props.push_back({prop, {}, cal::escape_text(v)});
  }
  if (std::string url = text(e, "url"); !url.empty()) {
    if (!url.starts_with("https://") && !url.starts_with("http://")) bad(field + ".url is an http(s) URL");
    c.props.push_back({"URL", {}, url});
  }
  for (auto [k, prop] : {std::pair{"status", "STATUS"}, std::pair{"transp", "TRANSP"}, std::pair{"class", "CLASS"}}) {
    std::string v = upper(text(e, k));
    if (v.empty()) continue;
    for (char ch : v) {
      if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-') bad(field + "." + k + " is one word, like CONFIRMED");
    }
    c.props.push_back({prop, {}, v});
  }
  if (std::string rr = text(e, "rrule"); !rr.empty()) {
    std::string err;
    auto rule = cal::parse_rule(rr, &err);
    if (!rule) bad(field + ".rrule: " + err);
    c.props.push_back({"RRULE", {}, cal::format_rule(*rule)});
  }
  for (auto [k, prop] : {std::pair{"exdates", "EXDATE"}, std::pair{"rdates", "RDATE"}}) {
    if (auto* a = e.if_contains(k)) {
      if (!a->is_array()) bad(field + "." + k + " is a list");
      for (const auto& v : a->as_array()) c.props.push_back(when_prop(prop, when_from(v, ctx, field + "." + k)));
    }
  }
  if (auto* rid = e.if_contains("recurrence_id"); rid && !rid->is_null()) {
    c.props.push_back(when_prop("RECURRENCE-ID", when_from(*rid, ctx, field + ".recurrence_id")));
  }
  if (const auto* org = object(e, "organizer")) c.props.push_back(person_prop("ORGANIZER", *org, field + ".organizer"));
  if (auto* a = e.if_contains("attendees")) {
    if (!a->is_array() || a->as_array().size() > 500) bad(field + ".attendees is a list of at most 500");
    for (const auto& x : a->as_array()) {
      if (!x.is_object()) bad(field + ".attendees holds {email, name?, partstat?, role?, rsvp?}");
      c.props.push_back(person_prop("ATTENDEE", x.as_object(), field + ".attendees"));
    }
  }
  if (auto* cats = e.if_contains("categories")) {
    if (!cats->is_array()) bad(field + ".categories is a list");
    std::string joined;
    for (const auto& x : cats->as_array()) {
      if (!x.is_string()) bad(field + ".categories holds text");
      joined += (joined.empty() ? "" : ",") + cal::escape_text(x.as_string());
    }
    if (!joined.empty()) c.props.push_back({"CATEGORIES", {}, joined});
  }
  if (auto* al = e.if_contains("alarms")) {
    if (!al->is_array() || al->as_array().size() > 10) bad(field + ".alarms is a list of at most 10");
    for (const auto& x : al->as_array()) {
      if (!x.is_object()) bad(field + ".alarms holds {trigger, action?, description?}");
      const auto& ao = x.as_object();
      std::string trigger = text(ao, "trigger", true);
      if (!cal::parse_duration(trigger)) bad(field + ".alarms trigger is a duration, like -PT30M");
      std::string action = upper(text(ao, "action"));
      if (action.empty()) action = "DISPLAY";
      if (action != "DISPLAY" && action != "AUDIO" && action != "EMAIL") bad(field + ".alarms action is DISPLAY, AUDIO, or EMAIL");
      std::string desc = text(ao, "description");
      if (desc.empty()) desc = text(e, "summary");
      c.children.push_back(cal::Component{"VALARM",
                                          {{"ACTION", {}, action},
                                           {"TRIGGER", {}, trigger},
                                           {"DESCRIPTION", {}, cal::escape_text(desc.empty() ? "Reminder" : desc)}},
                                          {}});
    }
  }
  return c;
}

cal::Component vcalendar(const std::string& method) {
  cal::Component c{"VCALENDAR", {{"PRODID", {}, kProdId}, {"VERSION", {}, "2.0"}, {"CALSCALE", {}, "GREGORIAN"}}, {}};
  if (!method.empty()) c.props.push_back({"METHOD", {}, method});
  return c;
}

std::string method_of(const bj::object& in, const char* k, std::initializer_list<const char*> allowed, const char* fallback) {
  std::string m = upper(text(in, k));
  if (m.empty()) return fallback;
  for (const char* a : allowed) {
    if (m == a) return m;
  }
  std::string list;
  for (const char* a : allowed) list += (list.empty() ? "" : ", ") + std::string(a);
  bad(std::string(k) + " is " + list);
}

struct Read {
  cal::Parsed parsed;
  std::vector<Cal> cals;
  std::vector<std::string> warnings;
};

Read read(const bj::object& in, const char* field = "ics") {
  std::string text_in = text(in, field, true);
  if (text_in.size() > cal::kMaxIcsBytes) throw Error{"too_large", std::string(field) + " is at most 1 MiB"};
  Read r;
  std::string err;
  auto parsed = cal::parse(text_in, &err);
  if (!parsed) bad(std::string(field) + ": " + err);
  r.parsed = std::move(*parsed);
  r.warnings = r.parsed.warnings;
  r.cals = calendars(r.parsed, &r.warnings);
  return r;
}

// The VEVENT a message is about: the one without RECURRENCE-ID, else the first.
const cal::Component* main_event(const Read& r, const Cal** which) {
  for (const auto& c : r.cals) {
    for (auto* e : c.events) {
      if (!e->prop("RECURRENCE-ID")) {
        *which = &c;
        return e;
      }
    }
  }
  for (const auto& c : r.cals) {
    if (!c.events.empty()) {
      *which = &c;
      return c.events.front();
    }
  }
  return nullptr;
}

bj::array warnings_json(const std::vector<std::string>& w) {
  bj::array a;
  for (size_t i = 0; i < w.size() && i < 50; ++i) a.push_back(bj::value(w[i]));
  return a;
}

// A reply-shaped VEVENT for one attendee, from the event as the organizer sent it.
cal::Component reply_event(const cal::Component& e, const std::string& attendee, const std::string& partstat,
                           const std::string& comment, Context& ctx) {
  cal::Component r{"VEVENT", {}, {}};
  for (const char* k : {"UID", "SEQUENCE", "RECURRENCE-ID", "ORGANIZER", "SUMMARY", "DTSTART", "DTEND", "DURATION"}) {
    for (auto* p : e.all(k)) r.props.push_back(*p);
  }
  if (!r.prop("SEQUENCE")) r.props.push_back({"SEQUENCE", {}, "0"});
  r.props.push_back({"DTSTAMP", {}, cal::when_value(cal::When{false, {}, "UTC", ctx.now_ms})});
  // The attendee's own line, kept from the invite when it's there (their CN, role).
  cal::Property mine{"ATTENDEE", {}, "mailto:" + attendee};
  for (auto* a : e.all("ATTENDEE")) {
    if (address_of(a->value) == attendee) {
      mine = *a;
      mine.value = "mailto:" + attendee;
    }
  }
  std::erase_if(mine.params, [](const cal::Param& p) { return p.name == "PARTSTAT" || p.name == "RSVP"; });
  mine.params.push_back({"PARTSTAT", {partstat}});
  r.props.push_back(std::move(mine));
  if (!comment.empty()) r.props.push_back({"COMMENT", {}, cal::escape_text(comment.substr(0, 2000))});
  return r;
}

std::string partstat_of(const bj::object& in) {
  std::string ps = upper(text(in, "partstat", true));
  if (ps != "ACCEPTED" && ps != "DECLINED" && ps != "TENTATIVE") bad("partstat is ACCEPTED, DECLINED, or TENTATIVE");
  return ps;
}

}  // namespace

bj::object ics_invite(const bj::object& in, Context& ctx) {
  const auto* ev = object(in, "event");
  if (!ev) bad("event is required");
  bj::object e = *ev;
  const auto* org = object(in, "organizer");
  if (!org) bad("organizer is {email, name?}");
  e["organizer"] = *org;
  auto* att = in.if_contains("attendees");
  if (!att || !att->is_array() || att->as_array().empty()) bad("attendees is a list of {email, name?}");
  bj::array people;
  for (const auto& a : att->as_array()) {
    if (!a.is_object()) bad("attendees holds {email, name?, role?}");
    bj::object p = a.as_object();
    p["partstat"] = "NEEDS-ACTION";
    p["rsvp"] = true;
    people.push_back(std::move(p));
  }
  e["attendees"] = std::move(people);
  std::string method = method_of(in, "method", {"REQUEST", "CANCEL"}, "REQUEST");
  e["status"] = method == "CANCEL" ? "CANCELLED" : (e.contains("status") ? e["status"] : bj::value("CONFIRMED"));
  auto calc = vcalendar(method);
  calc.children.push_back(event_component(e, ctx, "event"));
  std::string ics = cal::serialize(calc);
  // The event as a receiver will read it back.
  std::string err;
  auto parsed = cal::parse(ics, &err);
  std::vector<std::string> w;
  auto cals = calendars(*parsed, &w);
  return bj::object{{"ics", ics}, {"method", method}, {"event", event_json(*cals[0].events[0], cals[0], &w)}};
}

bj::object ics_interpret(const bj::object& in, Context& ctx) {
  ctx.used_tzdata = true;
  auto r = read(in);
  std::string me = address_of(text(in, "me"));
  const Cal* which = nullptr;
  const auto* e = main_event(r, &which);
  bj::object out;
  std::string method = which ? which->method : (r.cals.empty() ? "" : r.cals.front().method);
  out["method"] = method;
  if (!e) {
    out["kind"] = "other";
    out["warnings"] = warnings_json(r.warnings);
    return out;
  }
  auto ev = event_json(*e, *which, &r.warnings);
  out["uid"] = ev["uid"];
  out["sequence"] = ev["sequence"];
  if (ev.contains("comment")) out["comment"] = ev["comment"];
  // Who answered, for REPLY and COUNTER: the attendee the message is about.
  const auto& attendees = ev["attendees"].as_array();
  bj::object who = attendees.empty() ? bj::object{} : attendees.front().as_object();
  std::string kind = "other";
  if (method == "REQUEST" || method.empty()) {
    kind = ev["sequence"].as_int64() > 0 ? "update" : "invite";
    if (!me.empty()) {
      bool mine = false;
      for (const auto& a : attendees) {
        if (a.at("email").as_string() == me) {
          mine = true;
          out["my_partstat"] = a.at("partstat");
        }
      }
      out["for_me"] = mine;
    }
  } else if (method == "REPLY") {
    std::string ps = who.contains("partstat") ? std::string(who.at("partstat").as_string()) : "";
    kind = ps == "ACCEPTED" ? "accepted" : ps == "DECLINED" ? "declined" : ps == "TENTATIVE" ? "tentative"
           : ps == "DELEGATED" ? "delegated" : "reply";
  } else if (method == "COUNTER") {
    kind = "proposal";
    bj::object proposed;
    if (ev.contains("start")) proposed["start"] = ev["start"];
    if (ev.contains("end")) proposed["end"] = ev["end"];
    out["proposed"] = std::move(proposed);
  } else if (method == "DECLINECOUNTER") {
    kind = "counter_declined";
  } else if (method == "CANCEL") {
    kind = "cancelled";
  } else if (method == "REFRESH") {
    kind = "refresh";
  } else if (method == "PUBLISH") {
    kind = "published";
  } else if (method == "ADD") {
    kind = "added";
  }
  out["kind"] = kind;
  if (!who.empty() && method != "REQUEST" && method != "PUBLISH") {
    out["attendee"] = who.at("email");
    out["partstat"] = who.at("partstat");
  }
  out["event"] = std::move(ev);
  out["warnings"] = warnings_json(r.warnings);
  return out;
}

Library make_ics() {
  Library lib;
  lib.name = "ics";
  lib.major = 1;
  lib.description = "iCalendar (RFC 5545) as events in JSON, and invitations (RFC 5546): invite, reply, propose, "
                    "answer a proposal, cancel, expand recurrences.";
  auto& f = lib.functions;
  auto event_schema = any("{uid, sequence?, summary, description?, location?, url?, start, end?, rrule?, exdates?, "
                          "organizer?, attendees?, status?, categories?, alarms?}. start and end are {local, zone}, "
                          "{ms}, {utc}, RFC 3339 text, or {date} for all day.");

  f.push_back({"parse", "A calendar to events: zones resolved (its own VTIMEZONEs, IANA, Windows names), folding and "
                        "escaping undone, recurrence and attendees as fields.",
               schema({{"ics", str("The text/calendar body.")}}, {"ics"}),
               schema({{"method", str("")}, {"events", any("")}, {"warnings", any("")}}, {"events"}), {},
               [](const bj::object& in, Context& ctx) {
                 ctx.used_tzdata = true;
                 auto r = read(in);
                 bj::array events, todos;
                 std::string method, prodid;
                 for (const auto& c : r.cals) {
                   if (method.empty()) method = c.method, prodid = c.prodid;
                   for (auto* e : c.events) events.push_back(event_json(*e, c, &r.warnings));
                   for (auto* t : c.todos) {
                     todos.push_back(bj::object{{"uid", t->text("UID")}, {"summary", t->text("SUMMARY")},
                                                {"status", t->text("STATUS")}});
                   }
                 }
                 return bj::object{{"method", method}, {"prodid", prodid}, {"events", std::move(events)},
                                   {"todos", std::move(todos)}, {"warnings", warnings_json(r.warnings)}};
               }});

  f.push_back({"build", "Events to a calendar: CRLF, folded at 75 octets, TEXT escaped, TZIDs as IANA names.",
               schema({{"events", any("A list of events.")}, {"method", str("PUBLISH, REQUEST, REPLY, CANCEL, ... or none.")}},
                      {"events"}),
               schema({{"ics", str("")}}, {"ics"}), {},
               [](const bj::object& in, Context& ctx) {
                 auto* evs = in.if_contains("events");
                 if (!evs || !evs->is_array() || evs->as_array().empty() || evs->as_array().size() > 1000) {
                   bad("events is a list of 1 to 1000 events");
                 }
                 std::string method = method_of(in, "method", {"PUBLISH", "REQUEST", "REPLY", "ADD", "CANCEL", "REFRESH",
                                                               "COUNTER", "DECLINECOUNTER"}, "");
                 auto c = vcalendar(method);
                 size_t i = 0;
                 for (const auto& e : evs->as_array()) {
                   if (!e.is_object()) bad("events holds objects");
                   c.children.push_back(event_component(e.as_object(), ctx, "events[" + std::to_string(i++) + "]"));
                 }
                 return bj::object{{"ics", cal::serialize(c)}};
               }});

  f.push_back({"invite", "An invitation (METHOD:REQUEST, or CANCEL to withdraw it) from the organizer to attendees, "
                         "each NEEDS-ACTION with RSVP. Send its ics with mail's send {ics}; raise sequence to change it.",
               schema({{"event", event_schema}, {"organizer", any("{email, name?}")}, {"attendees", any("[{email, name?, role?}]")},
                       {"method", str("REQUEST (default) or CANCEL.")}},
                      {"event", "organizer", "attendees"}),
               schema({{"ics", str("")}, {"event", any("The event as receivers will read it.")}}, {"ics"}),
               bj::array{bj::object{{"in", bj::object{{"event", bj::object{{"uid", "meetup-7@example.com"}, {"summary", "Brisket night"},
                                                                         {"start", bj::object{{"local", "2026-11-05T18:00:00"}, {"zone", "America/Chicago"}}},
                                                                         {"end", bj::object{{"local", "2026-11-05T20:00:00"}, {"zone", "America/Chicago"}}}}},
                                                      {"organizer", bj::object{{"email", "thesis@example.com"}, {"name", "The Example Club"}}},
                                                      {"attendees", bj::array{bj::object{{"email", "ada@example.org"}}}}}},
                                    {"out", "{ics: BEGIN:VCALENDAR ... METHOD:REQUEST ..., event}"}}},
               [](const bj::object& in, Context& ctx) { return ics_invite(in, ctx); }});

  f.push_back({"respond", "An attendee's answer to an invitation: the REPLY (ACCEPTED, DECLINED, TENTATIVE), or with "
                          "propose {start, end}, a COUNTER asking for another time. Send it to the organizer.",
               schema({{"ics", str("The invitation.")}, {"attendee", str("Who answers: their address.")},
                       {"partstat", str("ACCEPTED, DECLINED, or TENTATIVE.")}, {"comment", str("")},
                       {"propose", any("{start, end?}: another time.")}},
                      {"ics", "attendee", "partstat"}),
               schema({{"ics", str("")}, {"method", str("REPLY or COUNTER.")}}, {"ics"}), {},
               [](const bj::object& in, Context& ctx) {
                 auto r = read(in);
                 const Cal* which = nullptr;
                 const auto* e = main_event(r, &which);
                 if (!e) bad("ics holds no event");
                 std::string who = address_of(text(in, "attendee", true));
                 if (!plain_address(who)) bad("attendee is one address");
                 std::string ps = partstat_of(in);
                 auto reply = reply_event(*e, who, ps, text(in, "comment"), ctx);
                 std::string method = "REPLY";
                 if (const auto* pr = object(in, "propose")) {
                   method = "COUNTER";
                   std::erase_if(reply.props, [](const cal::Property& p) {
                     return p.name == "DTSTART" || p.name == "DTEND" || p.name == "DURATION";
                   });
                   auto* s = pr->if_contains("start");
                   if (!s) bad("propose.start is required");
                   auto start = when_from(*s, ctx, "propose.start");
                   reply.props.push_back(when_prop("DTSTART", start));
                   if (auto* en = pr->if_contains("end")) {
                     auto end = when_from(*en, ctx, "propose.end");
                     if (end.ms < start.ms) bad("propose.end is after start");
                     reply.props.push_back(when_prop("DTEND", end));
                   }
                 }
                 auto c = vcalendar(method);
                 c.children.push_back(std::move(reply));
                 return bj::object{{"ics", cal::serialize(c)}, {"method", method}};
               }});

  f.push_back({"interpret", "What a calendar in a mail means, in one word (kind): invite, update, accepted, declined, "
                            "tentative, delegated, proposal (with proposed {start, end}), counter_declined, cancelled, "
                            "refresh, published; with uid, sequence, attendee, partstat, comment, and the event.",
               schema({{"ics", str("The text/calendar part.")}, {"me", str("The address it came to, for for_me.")}}, {"ics"}),
               schema({{"kind", str("")}, {"uid", str("")}, {"event", any("")}}, {"kind"}), {},
               [](const bj::object& in, Context& ctx) { return ics_interpret(in, ctx); }});

  f.push_back({"counter_decline", "Turns down a proposal: the DECLINECOUNTER to send back to the attendee who proposed.",
               schema({{"counter_ics", str("The COUNTER that came.")}, {"comment", str("Why, for them.")}}, {"counter_ics"}),
               schema({{"ics", str("")}, {"to", str("The proposer's address.")}}, {"ics"}), {},
               [](const bj::object& in, Context& ctx) {
                 auto r = read(in, "counter_ics");
                 const Cal* which = nullptr;
                 const auto* e = main_event(r, &which);
                 if (!e || which->method != "COUNTER") bad("counter_ics isn't a COUNTER");
                 cal::Component d{"VEVENT", {}, {}};
                 for (const char* k : {"UID", "SEQUENCE", "RECURRENCE-ID", "ORGANIZER", "ATTENDEE"}) {
                   for (auto* p : e->all(k)) d.props.push_back(*p);
                 }
                 d.props.push_back({"DTSTAMP", {}, cal::when_value(cal::When{false, {}, "UTC", ctx.now_ms})});
                 if (std::string c = text(in, "comment"); !c.empty()) d.props.push_back({"COMMENT", {}, cal::escape_text(c.substr(0, 2000))});
                 auto c = vcalendar("DECLINECOUNTER");
                 c.children.push_back(std::move(d));
                 std::string to = e->prop("ATTENDEE") ? address_of(e->prop("ATTENDEE")->value) : "";
                 return bj::object{{"ics", cal::serialize(c)}, {"to", to}};
               }});

  f.push_back({"counter_accept", "Takes a proposal: the event moved to the proposed times, sequence raised, every "
                                 "attendee asked again (NEEDS-ACTION), as a REQUEST to send to all of them.",
               schema({{"event", event_schema}, {"counter_ics", str("The COUNTER that came.")}}, {"event", "counter_ics"}),
               schema({{"ics", str("")}, {"event", any("")}}, {"ics", "event"}), {},
               [](const bj::object& in, Context& ctx) {
                 const auto* ev = object(in, "event");
                 if (!ev) bad("event is the event as you sent it (ics.parse or invite gives it)");
                 auto r = read(in, "counter_ics");
                 const Cal* which = nullptr;
                 const auto* e = main_event(r, &which);
                 if (!e || which->method != "COUNTER") bad("counter_ics isn't a COUNTER");
                 auto counter = event_json(*e, *which, &r.warnings);
                 bj::object moved = *ev;
                 if (counter["uid"] != moved["uid"]) bad("the COUNTER is for " + std::string(counter["uid"].as_string()) + ", not this event");
                 // The proposed start, and its end (or the event's own length, kept).
                 auto old_start = when_from(moved.at("start"), ctx, "event.start");
                 auto new_start = when_from(counter.at("start"), ctx, "counter.start");
                 moved["start"] = counter["start"];
                 if (counter.contains("end")) {
                   moved["end"] = counter["end"];
                 } else if (moved.contains("end")) {
                   auto old_end = when_from(moved.at("end"), ctx, "event.end");
                   moved["end"] = bj::object{{"ms", new_start.ms + (old_end.ms - old_start.ms)}};
                 }
                 moved["sequence"] = integer(moved, "sequence").value_or(0) + 1;
                 bj::array people;
                 if (auto* a = moved.if_contains("attendees"); a && a->is_array()) {
                   for (const auto& p : a->as_array()) {
                     bj::object q = p.as_object();
                     q["partstat"] = "NEEDS-ACTION";
                     q["rsvp"] = true;
                     people.push_back(std::move(q));
                   }
                 }
                 moved["attendees"] = std::move(people);
                 auto c = vcalendar("REQUEST");
                 c.children.push_back(event_component(moved, ctx, "event"));
                 std::string ics = cal::serialize(c);
                 std::string err;
                 auto back = cal::parse(ics, &err);
                 std::vector<std::string> w;
                 auto cals = calendars(*back, &w);
                 return bj::object{{"ics", ics}, {"event", event_json(*cals[0].events[0], cals[0], &w)}};
               }});

  f.push_back({"expand", "A recurring event's instances in a window, with its exceptions (EXDATE), extras (RDATE), and "
                         "moved instances (RECURRENCE-ID overrides) applied.",
               schema({{"ics", str("A calendar holding the event (and its overrides).")}, {"event", any("Or one event.")},
                       {"from", any("Instant.")}, {"to", any("Instant.")}, {"limit", num("Default 100, at most 10000.")}},
                      {"from", "to"}),
               schema({{"instances", any("[{uid, ms, utc, local, recurrence_id, override?}]")}, {"truncated", any("")}},
                      {"instances"}),
               {},
               [](const bj::object& in, Context& ctx) {
                 ctx.used_tzdata = true;
                 int64_t from = instant_field(in, "from"), to = instant_field(in, "to");
                 if (to <= from) bad("to is after from");
                 int64_t limit = std::clamp<int64_t>(integer(in, "limit").value_or(100), 1, kMaxInstances);
                 std::vector<bj::object> events;
                 if (in.contains("ics")) {
                   auto r = read(in);
                   for (const auto& c : r.cals) {
                     for (auto* e : c.events) events.push_back(event_json(*e, c, &r.warnings));
                   }
                 } else if (const auto* ev = object(in, "event")) {
                   events.push_back(*ev);
                 } else {
                   bad("give ics or event");
                 }
                 // Overrides by uid and original instant.
                 std::map<std::pair<std::string, int64_t>, bj::object> overrides;
                 for (const auto& e : events) {
                   if (e.contains("recurrence_id") && !e.at("recurrence_id").is_null()) {
                     auto rid = when_from(e.at("recurrence_id"), ctx, "recurrence_id");
                     overrides[{std::string(e.at("uid").as_string()), rid.ms}] = e;
                   }
                 }
                 bj::array out;
                 bool truncated = false;
                 for (const auto& e : events) {
                   if (e.contains("recurrence_id") && !e.at("recurrence_id").is_null()) continue;
                   if (!e.contains("start")) continue;
                   std::string uid(e.at("uid").as_string());
                   auto start = when_from(e.at("start"), ctx, "start");
                   std::shared_ptr<const cal::Zone> zone = start.zone.empty() || start.zone == "UTC" || start.date_only
                                                               ? cal::utc()
                                                               : zone_of(start.zone, ctx);
                   std::vector<int64_t> all;
                   if (std::string rr = e.contains("rrule") ? std::string(e.at("rrule").as_string()) : ""; !rr.empty()) {
                     std::string err;
                     auto rule = cal::parse_rule(rr, &err);
                     if (!rule) bad("rrule: " + err);
                     auto x = cal::expand(start.local, *rule, zone->to_local(to), size_t(kMaxInstances),
                                          [&](const cal::Civil& c) { return zone->to_utc(c, nullptr); });
                     for (const auto& c : x.instances) all.push_back(zone->to_utc(c, nullptr));
                     if (x.instances.size() >= size_t(kMaxInstances)) truncated = true;
                   } else {
                     all.push_back(start.ms);
                   }
                   if (auto* rd = e.if_contains("rdates"); rd && rd->is_array()) {
                     for (const auto& v : rd->as_array()) all.push_back(when_from(v, ctx, "rdates").ms);
                   }
                   std::set<int64_t> ex;
                   if (auto* xd = e.if_contains("exdates"); xd && xd->is_array()) {
                     for (const auto& v : xd->as_array()) ex.insert(when_from(v, ctx, "exdates").ms);
                   }
                   std::sort(all.begin(), all.end());
                   all.erase(std::unique(all.begin(), all.end()), all.end());
                   for (int64_t i : all) {
                     if (ex.count(i)) continue;
                     auto ov = overrides.find({uid, i});
                     int64_t at = i;
                     if (ov != overrides.end() && ov->second.contains("start")) at = when_from(ov->second.at("start"), ctx, "start").ms;
                     if (at < from || at >= to) continue;
                     if (int64_t(out.size()) >= limit) {
                       truncated = true;
                       break;
                     }
                     bj::object inst = at_zone(at, *zone);
                     inst["uid"] = uid;
                     inst["recurrence_id"] = utc_text(i);
                     if (ov != overrides.end()) inst["override"] = ov->second;
                     out.push_back(std::move(inst));
                   }
                 }
                 std::sort(out.begin(), out.end(), [](const bj::value& a, const bj::value& b) {
                   return a.at("ms").as_int64() < b.at("ms").as_int64();
                 });
                 return bj::object{{"instances", std::move(out)}, {"truncated", truncated}};
               }});
  return lib;
}

}  // namespace goober::libraries
