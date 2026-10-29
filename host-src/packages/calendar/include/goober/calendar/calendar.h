#pragma once

// Calendar invites by mail (design/machine/783.ThesisCommunity.md § 3): an iCalendar (RFC 5545)
// VEVENT a box's send carries as an iTIP REQUEST or CANCEL (RFC 5546), which mail clients show
// with RSVP buttons, and the REPLY that comes back when someone answers.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace goober::calendar {

struct Invite {
  std::string uid;          // stable per event, like "meetup-7"; the host adds @domain
  std::string start, end;   // RFC 3339 times ("2026-11-05T18:00:00-06:00", "...Z"), or dates for all day
  std::string summary, description, location, url;
  int64_t sequence = 0;     // raise it to change an event already sent
  bool cancel = false;      // METHOD:CANCEL
};

// The text/calendar body for one attendee, from the organizer, stamped at now_ms. nullopt with
// `error` for a bad field.
std::optional<std::string> invite_ics(const Invite& invite, std::string_view domain, std::string_view organizer_email,
                                      std::string_view organizer_name, std::string_view attendee_email, int64_t now_ms,
                                      std::string* error);

// What came back: METHOD:REPLY with the attendee's answer.
struct Reply {
  std::string method;    // REPLY, REQUEST, CANCEL, COUNTER, ...
  std::string uid;       // as sent, with its @domain
  std::string attendee;  // the address, lowercase
  std::string partstat;  // ACCEPTED, DECLINED, TENTATIVE, NEEDS-ACTION, DELEGATED
  int64_t sequence = 0;
  std::string comment;
};
std::optional<Reply> parse_reply(std::string_view ics);

}  // namespace goober::calendar
