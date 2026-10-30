# signing

Request signers that no API description can express (`design/machine/034.SpeakingHttp.md` § 3):
OAuth 1.0a (RFC 5849) with HMAC-SHA1.

## Quality bar

- **Checked against published examples**: X's documented signature for a signed POST.
- **RFC 3986 encoding exactly**: unreserved characters only, uppercase hex, parameters sorted by
  encoded name then value, query and form parameters included, the body's own bytes never.
