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
- A failed append is cut back off the log (ftruncate to the size before it), so a later
  bundle never sits behind torn bytes that recovery would stop at.
- Known gap (code review 4): when a committed bundle fails to apply (a full disk), commit
  returns false, but the bundle stays committed and is applied on the next start; the log
  stops truncating until then. A distinct "committed, apply pending" outcome would let the
  registry say so instead of reporting a refusal.
