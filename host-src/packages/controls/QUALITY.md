# controls

Per-domain operational controls (`design/machine/047.Controls.md`): the parsed and checked
values an operator sets (`spec.h`), the gate that applies them to a request and counts what each
control saw (`gate.h`), and the report that turns those counts into series, effects, and
verdicts (`report.h`). The host stores, changes, and wires them; this package has no disk and no
clock of its own.

## Quality bar

- **Strict in, nothing half applied.** An unknown key, a wrong type, a value out of range, or a
  match that doesn't parse is an error naming the key; a stored file compiles whole or not at all.
- **Only lower.** `body-max-bytes` and `timeout-ms` never pass the host's own limits, which stay
  the outer shield with the host-wide per-client limit.
- **Cheap per request.** Matchers are compiled once and sorted most specific first; a check is a
  walk of a domain's overrides and a few map lookups under one lock. No disk, no allocation per
  override.
- **Nothing leaks.** An allowed request's in-flight slots are held by a ticket released on
  `done`, or by its destructor when a finish path never calls it.
- **Observe refuses nothing.** An observed property counts what it would have refused on its own
  buckets, and never changes an answer.
- **No client is named.** Distinct clients are a 256-bit sketch; the busiest is a keyed hash. Raw
  client keys live only in memory, bounded (1024 a bucket, idle rate buckets forgotten after 15
  minutes).
- **Tests per behavior:** matcher specificity, per-property merge, each refusal and its status,
  observe, `by: user`, release on every path, reports and verdicts, and validation errors.
