# config

The single JSON host configuration: parsing, validation, and hot reload on SIGHUP.

## Quality bar

- Unknown fields are refused with their path, so a typo never silently does nothing.
- Every limit has a default and a ceiling, and `Limit::resolve` is the only place a
  manifest's request is clamped.
- Reload swaps a whole config atomically. Fields that need a restart keep their old
  values and are reported, never half-applied.
