# calendar

Calendar invites by mail (`design/machine/783.ThesisCommunity.md` § 3): RFC 5545 VEVENTs sent
as RFC 5546 REQUEST or CANCEL, and the REPLY an attendee's client sends back.

## Quality bar

- **What clients accept.** CRLF content lines folded at 75 octets (never inside a UTF-8
  sequence), TEXT escaped (`\\`, `;`, `,`, newlines), times in UTC, all-day events as dates.
- **No value can add a property.** Control characters are dropped and quotes are stripped from
  parameters, so a summary or a name can't break onto a line of its own.
- **Times carry their zone.** A time without `Z` or an offset is refused rather than guessed.
- **Replies are read leniently**: folded lines, any case, PARTSTAT anywhere in the attendee's
  parameters.
