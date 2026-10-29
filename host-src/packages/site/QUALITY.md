# site

Turns a tenant's content tree into a site: page kinds, URL resolution, front matter,
templates with layouts and partials, listings with sorting, paging, and tags, directory
listings, and Atom feeds (`design/machine/007.Content.md`).

## Quality bar

- **Private paths never leave.** Any segment starting with `_` is refused before anything
  is read, so templates and data files can't be fetched.
- **Requirements are enforced here and in listings.** A page with `requirements` is
  rendered only after the host's authorizer allows it, and a listing leaves out entries
  the visitor couldn't open, so a title can't leak through an index.
- **Templates can't inject markup.** Only `body`, which the host rendered, is unescaped.
  A box's JSON never supplies `body`.
- **Every entry in a listing defines the common keys** (url, title, date, summary, image,
  tags, order), so a Mustache section never falls through to the listing page's own value.
- No caching yet: every visit reads the files it needs, which keeps the behavior obvious.
- **Apps (PWA) are checked strictly.** `_apps/$name.json` names only manifest fields we know,
  paths on the site, and a scope outside the host's own paths; the generated worker never
  intercepts the host's paths and never stores a private or no-store response.
