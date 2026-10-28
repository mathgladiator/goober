# aws

Signature Version 4, S3 objects (put, get, list, delete), and SES v2 SendEmail, through curl
(design/machine/018.Mail.md and 019.Backups.md).

## Quality bar

- The signer is tested against AWS's published examples (the signing key derivation,
  get-vanilla, IAM ListUsers, S3 GET Object), and against curl's own `--aws-sigv4`: a
  stand-in service that checks signatures with this signer accepts what curl signs.
- Secrets never reach a command line: the signature, headers, and any session token go to
  curl in a 0600 config file that is removed when curl exits, and tests check none is left.
- Plain http goes only to loopback addresses (the stand-in in tests, a devbox's fake S3).
- Every failure says why, in the service's words when it gave any (S3's `<Code>`, SES's
  `message`), and SES refusals say whether asking again could help.
- The stand-in (`test/fake_aws.h`) checks every request's signature and pages listings three
  keys at a time, so paging and encoding are exercised; host tests reuse it.
