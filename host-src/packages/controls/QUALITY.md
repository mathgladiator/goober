# controls

Per-domain operational controls (`design/machine/047.Controls.md`): the parsed and checked
values an operator sets (`spec.h`), the gate that applies them to a request and counts what each
control saw (`gate.h`), and the report that turns those counts into series, effects, and
verdicts (`report.h`). MCP's strict rate and fixed queue (`queue.h`, 047 § 13) live here too: the
JSON-RPC call counter, the per-agent rate, the queue with its running caps, its rows, and its
report. The host stores, changes, and wires them; this package has no disk, and the queue's clock
is injected.

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
- **The queue is fixed and fair.** Its limits are constants, not settings. Each agent's calls
  run first in first out, freed slots go round-robin across queues and agents, and a queue and
  all MCP together run at most what the pool allows. A waiting call is a parked callback, never a
  thread. A slot is released once, by `release()` or its destructor. Every refusal is "rate limit
  reached", and nothing else.
- **Tests per behavior:** the call counter (batches, cheap methods, ids), the rate per agent, FIFO,
  round-robin, the caps, the wait limit, full, gone, release on every path, rows, health, the
  report, and a flood that doesn't starve a polite agent; matcher specificity, per-property merge, each refusal and its status,
  observe, `by: user`, release on every path, reports and verdicts, and validation errors.
