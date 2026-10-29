# panel

The operator panel's browser side (`design/machine/028.Panel.md` § 3): plain HTML, CSS, and
one ES2020 script, compiled into the binary by `cmake/Embed.cmake`. No build step and no
third party.

## Quality bar

- **No inline script or style**, so the panel's CSP (`script-src 'self'; style-src 'self'`)
  holds without exceptions. The tests check the HTML for `<script>` bodies and `style=`.
- **Tenant data is text, never markup.** Every value from the host (paths, names, rows,
  diffs) is placed with `textContent` or as an SVG text node; the script never assigns
  `innerHTML` (checked by the tests).
- **Look-only.** The script calls only `POST /api/$tool`, with JSON, and only read tools.
- Each asset carries a SHA-256 ETag computed at build time.
