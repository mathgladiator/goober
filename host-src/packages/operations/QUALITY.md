# operations

What the operator panel reads (`design/machine/028.Panel.md`): traffic rollups derived from
the access and egress logs, and the change log written from the host WAL's observer.

## Quality bar

- **The logs stay the record.** Rollups are derived: rows and the tailer's cursor commit in
  one transaction, so a line is counted once across crashes, restarts, and rotations, and a
  lost database can be rebuilt from the logs on disk.
- **No user data.** Rollups are keyed by tenant, box, and route name only: never a path, a
  user, or an address. Change rows name operators, never site visitors.
- **Bounded.** Minute rows are kept 48 hours, hour rows 400 days; change texts are kept only
  for content and schemas of at most 64 KiB, and dropped after 90 days. Old files past 1 MiB
  are measured, not read.
- **One meaning, one row.** A box upload's bundle is one deploy row; partial_put's kept
  copies are not rows of their own.
