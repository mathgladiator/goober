# calendar

The iCalendar engine under `ics@1` and `time@1` (`design/machine/831.Libraries.md`): civil time
and zones (IANA from the server's tzdata through C++20 chrono, fixed offsets, a calendar's own
VTIMEZONEs, Windows names), RFC 5545's content lines, typed values, and recurrence.

## Quality bar

- **Checked against the RFC.** RFC 5545 § 3.8.5.3's recurrence examples expand to the dates
  the RFC lists (18 of them), DST edges in New York and Chicago keep the wall clock, and
  Outlook's VTIMEZONE for Central time agrees with IANA at every transition tested.
- **Lenient in, strict out.** The parser reads folded lines, LF or CRLF, any case, quoted
  parameters, unclosed components, and unknown TZIDs (with warnings). The serializer writes
  CRLF, folds at 75 octets on UTF-8 boundaries, escapes TEXT, and quotes parameters only
  when they hold `:`, `;`, or `,`.
- **Bounded.** Input is at most 1 MiB, 8 components deep, 10,000 properties a component;
  expansion stops at its cap, its horizon, or 200,000 periods, and says it was truncated.
  Fuzzed in the `fuzz` package.
- **Gaps and overlaps are decided once**: a wall-clock time in a spring-forward gap lands
  after it, and one in a fall-back overlap is the earlier instant.
