# render

Front matter, Mustache templates, and markdown to HTML: the host code that turns content
into pages (070.UI.md, `design/machine/007.Content.md`). No I/O; pure functions of their
input, so every behavior is a table-driven test.

## Quality bar

- **Nothing a person typed can become markup.** Markdown escapes raw HTML unless the caller
  allows it, and drops link targets with unsafe schemes. Mustache escapes every value
  except the names the host marks as its own rendered HTML, looked up at the top of the data.
- **Templates are logic-less.** No helpers, no lambdas, no set-delimiter tags; a partial
  nests at most 16 deep, so a template that includes itself fails instead of looping.
- **Malformed input degrades, it doesn't throw.** Markdown always renders; a bad front
  matter line is reported and skipped; a malformed template is an error the caller shows.
- Markdown follows CommonMark for the common cases. Where it is simpler than the spec
  (lazy continuation inside nested containers, HTML block kinds), the test says so.
