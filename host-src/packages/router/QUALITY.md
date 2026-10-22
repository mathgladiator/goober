# router

Path normalization, route patterns (`/jobs/{id : int64}`), and the route table.

## Quality bar

- Normalization is the one gate between a raw request path and everything else. Every
  refusal it makes (traversal, encoded slashes, NUL, bad escapes) has a test.
- Precedence is deterministic: the segment count, then literal over parameter at the
  first difference. Conflicts are decided by shape alone, never by parameter type.
- The table is a linear scan. That is fine for tens of routes per domain; a trie can
  replace it behind the same interface when a benchmark asks for one.
