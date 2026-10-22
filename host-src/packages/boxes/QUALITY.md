# boxes

The registry of box versions: upload (check, store, activate), promote, and the immutable
active set with per-domain route tables that every request reads.

## Quality bar

- Nothing becomes active unless its manifest parses, its module compiles against the ABI,
  every route has its export, and no route conflicts or name collides with another box.
- An upload writes its version and its activation in one WAL bundle, so a crash leaves the
  old version active or the new one, never a missing file.
- Requests read a snapshot. An upload builds a new snapshot and swaps it in, so a request
  in flight never sees half a deployment.
- v1 activates on upload (the dev policy in design/machine/005.FirstVersion.md §9). The
  approval step goes here when it exists.
