# json

Compiles Boost.JSON exactly once and offers typed field readers whose errors name the
field. Every package that reads JSON from outside the host (configs, manifests, MCP)
goes through these readers so a person or agent always learns which field was wrong.

## Quality bar

- A wrong type is an error that names the field, never a silent default.
- `pretty` output always parses back to an equal value.
