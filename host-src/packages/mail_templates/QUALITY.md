# mail_templates

The templates every mail the host sends is rendered from (`design/machine/022.MailTemplates.md`):
a default for each of the host's own mails, overrides per tenant or for every tenant, a layout
around them, and templates operators add for boxes to send.

## Quality bar

- **Every default renders with its sample values**, and the test checks each one: no
  Mustache left over, no scripts, a doctype on the HTML.
- **Nothing a value holds becomes markup.** The HTML is rendered with every value escaped;
  only the layout's `{{{content}}}`, which the host rendered itself, is placed as it is. The
  subject and text are plain text, so values go in unescaped there and nothing reads them as HTML.
- **A template is checked before it is saved**: its name, sizes (64 KiB a part), a one-line
  subject, a text part, Mustache that parses, a layout with `{{{content}}}` in each part, and a
  render with the values the host will give it. A template that fails is never written.
- **No links in sign-in mails by default** (013): the test checks there is no `href`.
- **A broken layout can't stop a mail**: wrapping a box's text falls back to text only.
- Files are written with `write_file_durable`; names are `[a-z][a-z0-9-]*`, and tenants are
  checked, so no path can leave the store's directory.
