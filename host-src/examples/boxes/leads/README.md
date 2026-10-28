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

Each new signup gets a confirmation link through the host's `mail` service (declared in the
manifest): it leaves only once the signup is stored, through the outbox, at most once a day
to an address. With the dev mailer (the default) the mail is written to
`storage/mail/outbox/` and nobody receives it. To really send it, set the host's mailer to
Amazon SES (`identity.mailer: "ses"` with `identity.ses.from` a verified address, and the
AWS keys set with `goober-host key set aws_access_key_id` and `aws_secret_access_key`) or to
SMTP. See `design/machine/018.Mail.md`.
