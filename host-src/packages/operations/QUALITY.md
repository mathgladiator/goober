# operations

What the operator panel reads (`design/machine/028.Panel.md`): traffic rollups derived from
the access and egress logs (traffic, edges, and errors), the record of how box calls fail
(`failures.h`), and the change log written from the host WAL's observer.

## Quality bar

- **The logs stay the record.** Rollups are derived: rows and the tailer's cursor commit in
  one transaction, so a line is counted once across crashes, restarts, and rotations, and a
  lost database can be rebuilt from the logs on disk.
- **No user data.** Rollups are keyed by tenant, box, and route name only: never a user or an
  address. The one path kept is an error's place (028 § 4.2), and only the path of a page the
  site has; every path nothing answers is "(not found)". Change rows name operators, never
  site visitors. Box failures keep the host's own messages and boxes' error text, at most 300
  bytes each, the newest 200 per box for 30 days, as box logs keep them.
- **Bounded.** Minute rows are kept 48 hours, hour rows 400 days; change texts are kept only
  for content and schemas of at most 64 KiB, and dropped after 90 days. Old files past 1 MiB
  are measured, not read.
- **One meaning, one row.** A box upload's bundle is one deploy row; partial_put's kept
  copies are not rows of their own.
