# types

A box's types (036.Json.md): the `components.schemas` of an OpenAPI document, held to a
strict subset, and the checks the host runs with them. See design/machine/011.Types.md.

## Quality bar

- Anything outside the subset is refused by name at parse time: integers and numbers need a
  width, objects are closed (`"additionalProperties": false`), arrays have `maxItems`, a
  `$ref` stays in the document, and no type reaches itself.
- A check never trusts its input: unknown fields, nulls, wrong types, out-of-range numbers,
  overlong strings (counted in characters), and oversized arrays are each refused with the
  JSON path that failed.
- Defaults are filled in, and int64 values leave as strings (021), so a box sees one shape.
- Tests cover every refusal, every check, query text, and the JSON Schema agents are given.
