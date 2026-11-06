# manifest

Parses and checks the v1 subset of a box manifest (design/machine/005.FirstVersion.md §10).

## Quality bar

- Every refusal names the field and says what would be accepted, because an agent reads it.
- Fields from `150.DeepManifest.md` that v1 doesn't implement are accepted with a warning,
  never silently. Fields 150 doesn't define are refused.
- Route names and box names use the one grammar in `base`. Reserved prefixes come from one
  list here, mirrored from `041.ReservedRoutes.md` plus `/error-message`.
- Every route says who may call it: `"public": true` or `requirements`, never both or
  neither. Requirement roles are checked against the role grammar and the route's own
  parameters at upload, so `check_access` never sees a malformed role.
- Two maps are process-wide, set by the host from its config: beta domains' aliases
  (`set_domain_aliases`, read by `applies_to`) and beta tenants (`set_beta_tenants`, read by
  `DataStrategy::tenant_for`). Each thread reads its own copy, refreshed when the map's generation
  moves, so readers never contend. Two hosts in one process share them, so tests reset them.
- A beta request never opens production's data: a fixed tenant maps to its beta, and one with no
  beta to `kNoBetaTenant` + its name, which no database opens.
