// ics@1 against messages shaped as Gmail, Outlook, and Apple send them, and goober's own.

#include <gtest/gtest.h>

#include "goober/libraries/libraries.h"

using namespace goober;
namespace bj = boost::json;

namespace {

constexpr int64_t kNow = 1793286000000;  // 2026-10-29T15:00:00Z

bj::object call(const std::string& fn, const bj::object& in) {
  libraries::Context ctx{kNow};
  std::string out = libraries::call(*libraries::find("ics", 1), fn, json::dump(in), ctx);
  std::string err;
  auto v = json::parse(out, &err);
  EXPECT_TRUE(v && v->is_object()) << out;
  return v->as_object();
}

std::string crlf(std::string s) {
  std::string out;
  for (char c : s) {
    if (c == '\n') out += "\r\n";
    else out += c;
  }
  return out;
}

const std::string kGmailInvite = crlf(R"(BEGIN:VCALENDAR
PRODID:-//Google Inc//Google Calendar 70.9054//EN
VERSION:2.0
CALSCALE:GREGORIAN
METHOD:REQUEST
BEGIN:VTIMEZONE
TZID:America/Chicago
X-LIC-LOCATION:America/Chicago
BEGIN:DAYLIGHT
TZOFFSETFROM:-0600
TZOFFSETTO:-0500
TZNAME:CDT
DTSTART:19700308T020000
RRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=2SU
END:DAYLIGHT
BEGIN:STANDARD
TZOFFSETFROM:-0500
TZOFFSETTO:-0600
TZNAME:CST
DTSTART:19701101T020000
RRULE:FREQ=YEARLY;BYMONTH=11;BYDAY=1SU
END:STANDARD
END:VTIMEZONE
BEGIN:VEVENT
DTSTART;TZID=America/Chicago:20261105T180000
DTEND;TZID=America/Chicago:20261105T200000
DTSTAMP:20261029T150000Z
ORGANIZER;CN=Ann Lee:mailto:ann@gmail.com
UID:abc123@google.com
ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=
 TRUE;CN=thesis@kcmeatup.com;X-NUM-GUESTS=0:mailto:thesis@kcmeatup.com
ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;PARTSTAT=ACCEPTED;RSVP=TRUE
 ;CN=Ann Lee;X-NUM-GUESTS=0:mailto:ann@gmail.com
CREATED:20261029T145900Z
DESCRIPTION:Let's talk brisket.\nBring notes.
LAST-MODIFIED:20261029T150000Z
LOCATION:Q39\, Kansas City
SEQUENCE:0
STATUS:CONFIRMED
SUMMARY:Brisket summit
TRANSP:OPAQUE
BEGIN:VALARM
ACTION:DISPLAY
DESCRIPTION:This is an event reminder
TRIGGER:-P0DT0H30M0S
END:VALARM
END:VEVENT
END:VCALENDAR
)");

// Outlook's "Propose new time": a COUNTER, its zone a Windows name defined by its own VTIMEZONE.
const std::string kOutlookCounter = crlf(R"(BEGIN:VCALENDAR
METHOD:COUNTER
PRODID:Microsoft Exchange Server 2010
VERSION:2.0
BEGIN:VTIMEZONE
TZID:Central Standard Time
BEGIN:STANDARD
DTSTART:16010101T020000
TZOFFSETFROM:-0500
TZOFFSETTO:-0600
RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=1SU;BYMONTH=11
END:STANDARD
BEGIN:DAYLIGHT
DTSTART:16010101T020000
TZOFFSETFROM:-0600
TZOFFSETTO:-0500
RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=2SU;BYMONTH=3
END:DAYLIGHT
END:VTIMEZONE
BEGIN:VEVENT
ATTENDEE;PARTSTAT=TENTATIVE;CN=Bob Smith:mailto:Bob@Contoso.com
COMMENT;LANGUAGE=en-US:Thursday is tough\, can we do Friday?
SUMMARY;LANGUAGE=en-US:Brisket night
DTSTART;TZID=Central Standard Time:20261106T180000
DTEND;TZID=Central Standard Time:20261106T200000
UID:meetup-7@kcmeatup.com
CLASS:PUBLIC
PRIORITY:5
DTSTAMP:20261029T160000Z
TRANSP:OPAQUE
STATUS:CONFIRMED
SEQUENCE:0
LOCATION;LANGUAGE=en-US:Q39
END:VEVENT
END:VCALENDAR
)");

const std::string kAppleReply = crlf(R"(BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//Apple Inc.//iPhone 17.0//EN
METHOD:REPLY
BEGIN:VEVENT
UID:meetup-7@kcmeatup.com
DTSTART;TZID=America/Chicago:20261105T180000
DTEND;TZID=America/Chicago:20261105T200000
DTSTAMP:20261029T170000Z
SEQUENCE:0
SUMMARY:Brisket night
ORGANIZER;CN="The KC Thesis":mailto:thesis@kcmeatup.com
ATTENDEE;CN="Cara Diaz";PARTSTAT=DECLINED;ROLE=REQ-PARTICIPANT:mailto:cara@icloud.com
END:VEVENT
END:VCALENDAR
)");

bj::object meetup() {
  return bj::object{{"uid", "meetup-7@kcmeatup.com"},
                    {"summary", "Brisket night"},
                    {"location", "Q39, 1000 W 39th St"},
                    {"start", bj::object{{"local", "2026-11-05T18:00:00"}, {"zone", "America/Chicago"}}},
                    {"end", bj::object{{"local", "2026-11-05T20:00:00"}, {"zone", "America/Chicago"}}},
                    {"alarms", bj::array{bj::object{{"trigger", "-PT30M"}}}}};
}

}  // namespace

TEST(Ics, GmailsInviteReadsAsAnEvent) {
  auto p = call("parse", {{"ics", kGmailInvite}});
  EXPECT_EQ(p["method"], "REQUEST");
  ASSERT_EQ(p["events"].as_array().size(), 1u);
  const auto& e = p["events"].at(0).as_object();
  EXPECT_EQ(e.at("uid"), "abc123@google.com");
  EXPECT_EQ(e.at("summary"), "Brisket summit");
  EXPECT_EQ(e.at("description"), "Let's talk brisket.\nBring notes.");
  EXPECT_EQ(e.at("location"), "Q39, Kansas City");
  EXPECT_EQ(e.at("start").at("local"), "2026-11-05T18:00:00");
  EXPECT_EQ(e.at("start").at("zone"), "America/Chicago");
  EXPECT_EQ(e.at("start").at("utc"), "2026-11-06T00:00:00Z");
  EXPECT_EQ(e.at("end").at("utc"), "2026-11-06T02:00:00Z");
  EXPECT_EQ(e.at("organizer").at("email"), "ann@gmail.com");
  EXPECT_EQ(e.at("organizer").at("name"), "Ann Lee");
  ASSERT_EQ(e.at("attendees").as_array().size(), 2u);
  EXPECT_EQ(e.at("attendees").at(0).at("email"), "thesis@kcmeatup.com");
  EXPECT_EQ(e.at("attendees").at(0).at("partstat"), "NEEDS-ACTION");
  EXPECT_EQ(e.at("attendees").at(0).at("rsvp"), true);
  EXPECT_EQ(e.at("attendees").at(1).at("partstat"), "ACCEPTED");
  EXPECT_EQ(e.at("alarms").at(0).at("trigger"), "-P0DT0H30M0S");
  EXPECT_TRUE(e.at("other").as_object().contains("CREATED"));
  EXPECT_TRUE(p["warnings"].as_array().empty()) << json::dump(p["warnings"]);

  auto meaning = call("interpret", {{"ics", kGmailInvite}, {"me", "thesis@kcmeatup.com"}});
  EXPECT_EQ(meaning["kind"], "invite");
  EXPECT_EQ(meaning["for_me"], true);
  EXPECT_EQ(meaning["my_partstat"], "NEEDS-ACTION");
}

TEST(Ics, AcceptingAndDecliningAreRepliesTheOrganizerReads) {
  auto yes = call("respond", {{"ics", kGmailInvite}, {"attendee", "thesis@kcmeatup.com"}, {"partstat", "accepted"},
                              {"comment", "We'll be there"}});
  ASSERT_FALSE(yes.contains("error")) << json::dump(yes);
  EXPECT_EQ(yes["method"], "REPLY");
  std::string ics(yes["ics"].as_string());
  EXPECT_NE(ics.find("METHOD:REPLY\r\n"), std::string::npos);
  EXPECT_NE(ics.find("UID:abc123@google.com\r\n"), std::string::npos);
  EXPECT_NE(ics.find("ORGANIZER;CN=Ann Lee:mailto:ann@gmail.com"), std::string::npos) << ics;
  auto read = call("interpret", {{"ics", ics}});
  EXPECT_EQ(read["kind"], "accepted");
  EXPECT_EQ(read["attendee"], "thesis@kcmeatup.com");
  EXPECT_EQ(read["comment"], "We'll be there");
  ASSERT_EQ(read["event"].at("attendees").as_array().size(), 1u);  // only the one who answers

  auto apple = call("interpret", {{"ics", kAppleReply}});
  EXPECT_EQ(apple["kind"], "declined");
  EXPECT_EQ(apple["attendee"], "cara@icloud.com");
  EXPECT_EQ(apple["uid"], "meetup-7@kcmeatup.com");
  EXPECT_EQ(call("respond", {{"ics", kGmailInvite}, {"attendee", "x@y.z"}, {"partstat", "MAYBE"}})["code"], "bad_input");
}

TEST(Ics, InvitesCancellationsAndRoundTrips) {
  auto inv = call("invite", {{"event", meetup()},
                             {"organizer", bj::object{{"email", "thesis@kcmeatup.com"}, {"name", "The KC Thesis"}}},
                             {"attendees", bj::array{bj::object{{"email", "Ada@Example.org"}, {"name", "Ada"}},
                                                     bj::object{{"email", "bob@contoso.com"}}}}});
  ASSERT_FALSE(inv.contains("error")) << json::dump(inv);
  std::string ics(inv["ics"].as_string());
  for (const char* want : {"METHOD:REQUEST\r\n", "PRODID:-//goober//ics@1//EN\r\n", "DTSTART;TZID=America/Chicago:20261105T180000\r\n",
                           "DTSTAMP:20261029T150000Z\r\n", "LOCATION:Q39\\, 1000 W 39th St\r\n", "STATUS:CONFIRMED\r\n",
                           "BEGIN:VALARM\r\n", "TRIGGER:-PT30M\r\n"}) {
    EXPECT_NE(ics.find(want), std::string::npos) << want << "\n" << ics;
  }
  // Every line fits 75 octets.
  for (size_t s = 0, e; s < ics.size(); s = e + 2) {
    e = ics.find("\r\n", s);
    EXPECT_LE(e - s, 75u) << ics.substr(s, e - s);
  }
  const auto& back = inv["event"].as_object();
  EXPECT_EQ(back.at("start").at("utc"), "2026-11-06T00:00:00Z");
  EXPECT_EQ(back.at("attendees").at(0).at("email"), "ada@example.org");
  EXPECT_EQ(back.at("attendees").at(0).at("partstat"), "NEEDS-ACTION");
  EXPECT_EQ(back.at("location"), "Q39, 1000 W 39th St");
  // As the attendee's client sees it.
  auto seen = call("interpret", {{"ics", ics}, {"me", "bob@contoso.com"}});
  EXPECT_EQ(seen["kind"], "invite");
  EXPECT_EQ(seen["for_me"], true);
  // A parse, a build, and a parse again keep the event.
  auto again = call("build", {{"method", "PUBLISH"}, {"events", call("parse", {{"ics", ics}})["events"]}});
  auto twice = call("parse", {{"ics", again["ics"]}});
  EXPECT_EQ(twice["events"].at(0).at("start"), back.at("start"));
  EXPECT_EQ(twice["events"].at(0).at("summary"), "Brisket night");

  auto cancel = call("invite", {{"event", meetup()}, {"organizer", bj::object{{"email", "thesis@kcmeatup.com"}}},
                                {"attendees", bj::array{bj::object{{"email", "ada@example.org"}}}}, {"method", "cancel"}});
  EXPECT_EQ(call("interpret", {{"ics", cancel["ics"]}})["kind"], "cancelled");
  EXPECT_NE(std::string(cancel["ics"].as_string()).find("STATUS:CANCELLED"), std::string::npos);

  for (const auto& bad : {bj::object{{"summary", "x"}, {"start", "2026-11-05T18:00:00Z"}},  // no uid
                          bj::object{{"uid", "a"}, {"start", "soon"}},
                          bj::object{{"uid", "a"}, {"start", bj::object{{"local", "2026-11-05T18:00:00"}}}},  // no zone
                          bj::object{{"uid", "a"}, {"start", "2026-11-05T18:00:00Z"}, {"end", "2026-11-05T17:00:00Z"}},
                          bj::object{{"uid", "a"}, {"start", "2026-11-05T18:00:00Z"}, {"rrule", "FREQ=NEVER"}}}) {
    auto r = call("build", {{"events", bj::array{bad}}});
    EXPECT_EQ(r["code"], "bad_input") << json::dump(bad);
  }
}

TEST(Ics, ProposalsAreCountersTheOrganizerTakesOrTurnsDown) {
  // Outlook proposes Friday instead.
  auto p = call("interpret", {{"ics", kOutlookCounter}, {"me", "thesis@kcmeatup.com"}});
  EXPECT_EQ(p["kind"], "proposal");
  EXPECT_EQ(p["attendee"], "bob@contoso.com");
  EXPECT_EQ(p["comment"], "Thursday is tough, can we do Friday?");
  // "Central Standard Time", from the message's own VTIMEZONE: Friday 18:00 CST is 00:00Z.
  EXPECT_EQ(p["proposed"].at("start").at("utc"), "2026-11-07T00:00:00Z");
  EXPECT_EQ(p["proposed"].at("end").at("utc"), "2026-11-07T02:00:00Z");

  // Taken: the event moves, its sequence rises, and everyone is asked again.
  bj::object event = meetup();
  event["organizer"] = bj::object{{"email", "thesis@kcmeatup.com"}};
  event["attendees"] = bj::array{bj::object{{"email", "bob@contoso.com"}, {"partstat", "ACCEPTED"}},
                                 bj::object{{"email", "ada@example.org"}, {"partstat", "ACCEPTED"}}};
  auto take = call("counter_accept", {{"event", event}, {"counter_ics", kOutlookCounter}});
  ASSERT_FALSE(take.contains("error")) << json::dump(take);
  EXPECT_EQ(take["event"].at("sequence").as_int64(), 1);
  EXPECT_EQ(take["event"].at("start").at("utc"), "2026-11-07T00:00:00Z");
  EXPECT_EQ(take["event"].at("attendees").at(1).at("partstat"), "NEEDS-ACTION");
  EXPECT_EQ(call("interpret", {{"ics", take["ics"]}})["kind"], "update");

  // Or turned down, to Bob.
  auto no = call("counter_decline", {{"counter_ics", kOutlookCounter}, {"comment", "Thursday it is"}});
  EXPECT_EQ(no["to"], "bob@contoso.com");
  auto read = call("interpret", {{"ics", no["ics"]}});
  EXPECT_EQ(read["kind"], "counter_declined");
  EXPECT_EQ(read["comment"], "Thursday it is");
  EXPECT_EQ(call("counter_decline", {{"counter_ics", kAppleReply}})["code"], "bad_input");  // not a COUNTER

  // goober's own attendees can propose too: respond with propose.
  auto mine = call("respond", {{"ics", take["ics"]}, {"attendee", "ada@example.org"}, {"partstat", "TENTATIVE"},
                               {"propose", bj::object{{"start", bj::object{{"local", "2026-11-07T12:00:00"}, {"zone", "America/Chicago"}}}}}});
  EXPECT_EQ(mine["method"], "COUNTER");
  auto theirs = call("interpret", {{"ics", mine["ics"]}});
  EXPECT_EQ(theirs["kind"], "proposal");
  EXPECT_EQ(theirs["attendee"], "ada@example.org");
  EXPECT_EQ(theirs["proposed"].at("start").at("utc"), "2026-11-07T18:00:00Z");
}

TEST(Ics, ExpandAppliesExceptionsAndMovedInstances) {
  std::string ics = crlf(R"(BEGIN:VCALENDAR
VERSION:2.0
BEGIN:VEVENT
UID:dinner@kcmeatup.com
DTSTART;TZID=America/Chicago:20261029T190000
DTEND;TZID=America/Chicago:20261029T210000
RRULE:FREQ=WEEKLY;BYDAY=TH;COUNT=4
EXDATE;TZID=America/Chicago:20261105T190000
SUMMARY:Thursday dinner
END:VEVENT
BEGIN:VEVENT
UID:dinner@kcmeatup.com
RECURRENCE-ID;TZID=America/Chicago:20261112T190000
DTSTART;TZID=America/Chicago:20261113T190000
DTEND;TZID=America/Chicago:20261113T210000
SUMMARY:Thursday dinner (on Friday)
END:VEVENT
END:VCALENDAR
)");
  auto x = call("expand", {{"ics", ics}, {"from", "2026-10-01T00:00:00Z"}, {"to", "2026-12-01T00:00:00Z"}});
  ASSERT_FALSE(x.contains("error")) << json::dump(x);
  const auto& inst = x["instances"].as_array();
  ASSERT_EQ(inst.size(), 3u) << json::dump(x);  // Oct 29, Nov 13 (moved), Nov 19; Nov 5 excepted
  EXPECT_EQ(inst[0].at("local"), "2026-10-29T19:00:00");
  EXPECT_EQ(inst[1].at("local"), "2026-11-13T19:00:00");
  EXPECT_EQ(inst[1].at("override").at("summary"), "Thursday dinner (on Friday)");
  EXPECT_EQ(inst[2].at("local"), "2026-11-19T19:00:00");
}

TEST(Ics, WhatIsntACalendarIsRefusedAndOddInputIsSurvived) {
  EXPECT_EQ(call("parse", {{"ics", "hello"}})["code"], "bad_input");
  EXPECT_EQ(call("parse", {{"ics", std::string((1 << 20) + 1, 'x')}})["code"], "too_large");
  // Unknown TZID, floating time, unclosed component: read with warnings, never thrown.
  auto odd = call("parse", {{"ics", "BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:x\nDTSTART;TZID=Mars Standard Time:20261105T180000\n"
                                    "DTEND:20261105T190000\nSUMMARY:odd\n"}});
  ASSERT_FALSE(odd.contains("error")) << json::dump(odd);
  EXPECT_EQ(odd["events"].at(0).at("summary"), "odd");
  EXPECT_GE(odd["warnings"].as_array().size(), 2u);
}
