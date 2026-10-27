# leads

Collects emails and survey answers from a landing page (an ad's destination), and gives the
owner's agent tools to count, tally, and export them. See `READINESS.md` at the repo root.

## Use it

1. Deploy: `just examples` stores `leads1` and deploys the box, along with the sample landing
   page `beef-share.html` and its `thanks` and `confirmed` pages.
2. Make your own landing page: a whole `.html` document (inline scripts and pixels work there)
   whose form posts to `/leads/<campaign>/signup` with `email`, `consent`, optional `name`,
   survey answers as `q_<question>` fields, hidden `utm_source`, `utm_medium`,
   `utm_campaign`, and `utm_content` fields, the `website` honeypot, and `next` (a path on
   this site to land on). Copy `examples/site/beef-share.html`.
3. Point the ad at `https://your.domain/your-page?utm_source=facebook&utm_content=<ad name>`.
4. Ask your agent: `leads_stats` (which ad works), `survey_summary`, `leads_list`,
   `leads_export`; or download `/leads/v1/export/<campaign>` signed in as the owner.

## Confirmation mail

Without a mail service, signups are stored unconfirmed. To send each new signup a
confirmation link through the outbox (only once the signup is stored, retried with one
idempotency key), add a `mail` service to the manifest. For Resend:

```json
"external": {
  "mail": {
    "base": "https://api.resend.com",
    "credentials": "keys/system/resend",
    "methods": {
      "send": {
        "verb": "post",
        "path": "/emails",
        "effect": "outbox",
        "idempotency": "header/Idempotency-Key",
        "require": { "body.from": "Your Name <hello@your.domain>" }
      }
    }
  }
}
```

then `key_set scope=system name=resend value=re_...` and deploy again. The box sends
`{"to": [...], "subject", "text"}`, Resend's shape; another provider needs its own field
names (or a small adapter box).
