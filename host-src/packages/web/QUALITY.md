# web

Reading the web for the crawler (800.Crawl.md, design/machine/017.Crawl.md): URLs, robots.txt,
a tolerant HTML parser, and the views that simplify a page: text, markdown, links, meta,
JSON-LD, tables, CSS selection, and extraction.

## Quality bar

- Every input is hostile: a page anyone wrote, a robots.txt, a selector an agent typed. Nothing
  recurses without a bound (depth 256, 200,000 nodes), every view caps its output, and selector
  matching is memoized so nested descendant chains stay polynomial.
- URLs are http or https only, with no user info or control bytes, dot segments removed, and
  one normal form (`Url::str`) used as a page's key.
- robots.txt follows RFC 9309: our product token's groups merged, else `*`; longest match
  wins, ties allow; `/robots.txt` is always allowed; a 5xx means disallow everything.
- The parser handles what real pages are: unclosed paragraphs and list items, stray end
  tags, unquoted attributes, raw script and style text. It is not the HTML5 algorithm.
- Tests cover each view on a realistic page, every selector feature and refusal, and hostile
  input (deep nesting, a megabyte of `<`); the fuzz package mutates all of it.
