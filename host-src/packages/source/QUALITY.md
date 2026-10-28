# source

Source projects kept in the host (design/human/521.Source.md; design/machine/020.Source.md):
a straight line of numbered revisions per project, each a whole snapshot, file contents stored
once by SHA-256, and unified diffs.

## Quality bar

- **History is never rewritten.** A revision, once committed, is never changed or removed;
  going back is a new revision (`revert`).
- **A commit lands whole or not at all**, in one SQLite transaction, and only on top of the
  newest revision. A commit from an older base lands only when no later revision touched any
  of its files; otherwise it is refused, naming the files.
- **Paths are safe to write out**: relative, no `.` or `..` or empty segments, no NUL,
  backslash, or line break, and a file can't sit where a directory is.
- **Bounded**: 4 MiB a file, 10,000 files and 256 MiB a project, 500 changes a commit (the
  tool), and diffs fall back to whole-file past 50,000 lines or 2,000 edits, so no input
  costs unbounded time or memory.
- Diffs match `diff -u`'s hunks; the tests check them against it.
