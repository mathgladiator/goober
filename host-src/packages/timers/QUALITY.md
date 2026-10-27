# timers

Cron schedules computed in a time zone (045.Time.md; design/machine/014.Time.md), using the
C++20 time zone database the system provides.

## Quality bar

- Five fields with *, numbers, ranges, lists, and steps; anything else is refused with the
  field named.
- Daylight saving follows 045: a local time that doesn't exist fires at the next one that
  does, and one that happens twice fires only the first time. Tests pin both transitions.
- `next` is strictly after, so asking at a fire's own time never repeats it; schedules that
  never fire (like February 31st) end within eight years rather than looping.
- Pure functions of their inputs: the scheduler injects the clock, so a year of schedules
  runs in a test in milliseconds.
