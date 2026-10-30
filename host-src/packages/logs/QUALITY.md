# logs

The host's logs (`design/machine/032.Logs.md`): one JSON object per line with a common
envelope, a hash chain through every line, rotation by day and size with a manifest per
rotated file, verification, and search.

## Quality bar

- **One chain.** Every line's `chain` covers the line before it, across rotations and across
  processes (writers take `flock` and re-read the tail when another process wrote).
- **Never lose a line to rotation.** A writer that opened the file before it was rotated
  notices (the inode changed) and writes to the new file.
- **Deterministic bytes.** The chain is computed over the exact bytes written, so verifying
  never depends on re-serializing JSON.
- **Bounded.** Search reads at most what it's told, newest first, and says when it stopped.
