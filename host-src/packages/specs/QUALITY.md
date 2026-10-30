# specs

Pinned OpenAPI documents (`design/machine/034.SpeakingHttp.md` § 2): the operations a
manifest's methods import by `operationId` or `VERB /path`.

## Quality bar

- **Pinned.** A spec is stored by its SHA-256 and named `name@sha12`; nothing is fetched while a
  call is made, and a changed spec is a new pin.
- **Only what a call needs.** Verb, path template, parameter places, and request body types,
  from OpenAPI 2.0 (Swagger), 3.0, 3.1, and 3.2 in JSON; local `$ref`s to parameters and
  bodies resolved. Security schemes are ignored: the manifest decides.
- **Checked when put**, so a bad spec fails at `spec_put`, not at the first call.
