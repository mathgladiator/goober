#include <gtest/gtest.h>

#include "goober/calendar/calendar.h"

using namespace goober::calendar;

namespace {
constexpr int64_t kNow = 1793288400000;  // 2026-10-29T15:40:00Z
}

TEST(Calendar, AnInviteIsAnRfc5545RequestForOneAttendee) {
  Invite inv{"meetup-7", "2026-11-05T18:00:00-06:00", "2026-11-05T20:00:00-06:00", "Brisket night, decided",
             "We picked Q39.\nBring cash; they split checks.", "Q39, 1000 W 39th St, Kansas City", "https://kcmeatup.com/m/7"};
  std::string err;
  auto ics = invite_ics(inv, "kcmeatup.com", "thesis@kcmeatup.com", "The KC Thesis", "ada@example.com", kNow, &err);
  ASSERT_TRUE(ics) << err;
  std::string unfolded = *ics;
  for (size_t p; (p = unfolded.find("\r\n ")) != std::string::npos;) unfolded.erase(p, 3);
  for (const char* want : {"METHOD:REQUEST\r\n", "UID:meetup-7@kcmeatup.com\r\n", "DTSTAMP:20261029T154000Z\r\n",
                           "DTSTART:20261106T000000Z\r\n", "DTEND:20261106T020000Z\r\n",
                           "SUMMARY:Brisket night\\, decided\r\n", "DESCRIPTION:We picked Q39.\\nBring cash\\; they split checks.\r\n",
                           "ORGANIZER;CN=\"The KC Thesis\":mailto:thesis@kcmeatup.com\r\n",
                           "ATTENDEE;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:ada@example.com\r\n",
                           "STATUS:CONFIRMED\r\n", "END:VCALENDAR\r\n"}) {
    EXPECT_NE(unfolded.find(want), std::string::npos) << want << "\n" << *ics;
  }
  // No content line passes 75 octets.
  for (size_t s = 0, e; s < ics->size(); s = e + 2) {
    e = ics->find("\r\n", s);
    EXPECT_LE(e - s, 75u);
  }
  Invite day{"picnic", "2026-11-07", "2026-11-08", "Picnic", "", "", "", 2, true};
  auto cancel = invite_ics(day, "kcmeatup.com", "thesis@kcmeatup.com", "", "ada@example.com", kNow, &err);
  ASSERT_TRUE(cancel) << err;
  EXPECT_NE(cancel->find("METHOD:CANCEL"), std::string::npos);
  EXPECT_NE(cancel->find("DTSTART;VALUE=DATE:20261107"), std::string::npos);
  EXPECT_NE(cancel->find("SEQUENCE:2"), std::string::npos);
  EXPECT_NE(cancel->find("STATUS:CANCELLED"), std::string::npos);
  for (Invite bad : {Invite{"has space", "2026-11-05", "", "x"}, Invite{"a", "2026-11-05T18:00:00", "", "x"},
                     Invite{"a", "2026-11-05T18:00:00Z", "2026-11-05", "x"}, Invite{"a", "2026-11-05", "2026-11-04", "x"},
                     Invite{"a", "2026-11-05", "", ""}, Invite{"a", "2026-11-05", "", "x", "", "", "http://x"}}) {
    EXPECT_FALSE(invite_ics(bad, "d.com", "o@d.com", "", "a@b.com", kNow, &err)) << bad.uid << " " << bad.start;
  }
  // A long description is folded, and each folded line still fits.
  Invite longer{"l", "2026-11-05", "", "x", std::string(300, 'y') + "\xC3\xA9"};
  auto folded = invite_ics(longer, "d.com", "o@d.com", "", "a@b.com", kNow, &err);
  ASSERT_TRUE(folded);
  EXPECT_NE(folded->find("\r\n "), std::string::npos);
}

TEST(Calendar, TheReplyThatComesBackSaysWhoAndWhat) {
  // As Gmail sends it: folded, mixed case, the attendee's answer in PARTSTAT.
  std::string ics =
      "BEGIN:VCALENDAR\r\nMETHOD:REPLY\r\nPRODID:Google Calendar\r\nBEGIN:VEVENT\r\nDTSTART:20261106T000000Z\r\n"
      "ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;partstat=Accepted;CN=Ada\r\n Lovelace:mailto:Ada@Example.com\r\n"
      "UID:meetup-7@kcmeatup.com\r\nSEQUENCE:0\r\nCOMMENT:See you there\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  auto r = parse_reply(ics);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->method, "REPLY");
  EXPECT_EQ(r->uid, "meetup-7@kcmeatup.com");
  EXPECT_EQ(r->attendee, "ada@example.com");
  EXPECT_EQ(r->partstat, "ACCEPTED");
  EXPECT_EQ(r->comment, "See you there");
  EXPECT_FALSE(parse_reply("not a calendar"));
}
