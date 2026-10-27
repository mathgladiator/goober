# fuzz

Seeded mutation fuzzing of every parser that reads text a person, an agent, or a box
controls, and a property test of the SQL authorizer (design/machine/013.SecurityReview.md).
No library: inputs grow from a seed corpus by inserting each target's interesting tokens,
repeating ranges (to find depth and quadratic blowups), flipping and inserting bytes, and
cutting.

## Quality bar

- Every target runs a few thousand cases in the ordinary suite, deterministic by seed.
  `GOOBER_FUZZ_ITERS=50000` asks for more; `just fuzz` runs that under AddressSanitizer and
  UndefinedBehaviorSanitizer (the `asan` preset).
- A target fails on a crash or on any one input taking more than three seconds.
- The SQL property: thousands of hostile statements through a strategy granted only `open`
  never change `secret` and never return its values, directly or through `open`, and a
  statement that makes SQLite roll back by itself spoils the request rather than letting its
  later writes commit one by one. Both rules were found by this test.
