# wal

The host WAL: every file the host owns changes through a bundle that is logged,
committed, applied, and marked done, so a crash never leaves half a change.

## Quality bar

- Every crash point is tested: before the commit record (discarded), after it (replayed),
  and a torn tail (stops cleanly at the last good record).
- Applying a bundle is idempotent, so replay is always safe.
- Paths are checked twice: when logged and when applied. Nothing escapes the storage root.
- One mutex serializes commits. That is slow and simple on purpose, and a group commit
  can replace it when a benchmark asks for one.
