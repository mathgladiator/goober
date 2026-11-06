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
- An upload activates unless it is stored dark (`store`) or for beta; approvals live in the
  host, not here.
- Beta (053): each box's beta slot answers on beta domains only. Production wins: a version going
  live clears a beta version in its way (said in the upload's warnings), and a reload leaves out
  only the beta versions that conflict. A version first stored for beta says so in its life
  record (`channel: "beta"`), erased when the same bytes are stored for production.
- Splits (052): starting and changing are told apart under the lock (`split_set`'s `existing`),
  a split's promotion runs only while its split does, and a split a new live version or a
  disable ends keeps its record as `split-last`.
