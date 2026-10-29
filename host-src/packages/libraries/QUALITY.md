# libraries

Closed host libraries a manifest imports and a box calls through `goober_lib`
(`design/machine/831.Libraries.md`): `time@1` and `ics@1`, with their JSON contracts.

## Quality bar

- **Pure.** A function reads its argument and the request's clock (and tzdata, named in its
  answer); it writes nothing and calls nothing. The same input gives the same answer.
- **Never a trap.** Every failure is `{"error", "code"}` (unknown_function, bad_input,
  too_large, limit); exceptions stop at `call`.
- **Contracts are data.** Each function carries its input and output schema and examples,
  served by `library_get`; within a major version, functions and optional fields are only
  added.
- **Real messages.** `ics@1` is tested against Gmail-, Outlook-, and Apple-shaped REQUEST,
  REPLY, and COUNTER messages, round trips, and goober's own invites, replies, and proposals.
