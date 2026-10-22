# manifest

Parses and checks the v1 subset of a box manifest (design/machine/005.FirstVersion.md §10).

## Quality bar

- Every refusal names the field and says what would be accepted, because an agent reads it.
- Fields from `150.DeepManifest.md` that v1 doesn't implement are accepted with a warning,
  never silently. Fields 150 doesn't define are refused.
- Route names and box names use the one grammar in `base`. Reserved prefixes come from one
  list here, mirrored from `041.ReservedRoutes.md` plus `/error-message`.
