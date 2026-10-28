# mail_inbound

Received mail as a box sees it (038.Mail.md, `design/machine/024.MailIn.md`): mail route
address patterns and their matching, a MIME parser, and the test for automatic mail. No I/O:
pure functions of their input, each table-tested.

## Quality bar

- **Bad input degrades; nothing throws.** Missing boundaries, no closing boundary, junk
  base64, unknown charsets, and empty messages all parse to what can be read. Invalid UTF-8
  becomes valid.
- **Bounded.** Multipart nesting stops at depth 8 and 100 parts, each body at 1 MiB of text,
  and the message says it was truncated. An address's local part is at most 64 characters,
  and a parameter's value too.
- **Decoding is exact where it matters**: RFC 2047 encoded words (B and Q, adjacent words
  joined), RFC 2231 parameters (charset, continuations), base64 with any line breaks,
  quoted-printable with soft breaks, and UTF-8, US-ASCII, ISO-8859-1, and Windows-1252.
- **Patterns are strict** about what they accept (lowercase, RFC 5322 atext, one token per
  address, a literal between parameters), so two patterns either clearly differ or share a
  shape, which the registry refuses across boxes.
- Automatic mail follows RFC 3834 (Auto-Submitted), the common X-Autoreply headers,
  Precedence, delivery reports, and empty envelope senders.
