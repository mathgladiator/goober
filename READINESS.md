# Readiness

> How ready goober is for the human's experiments, as of 2026-10-29. Short on
> purpose: `PROGRESS.md` has the full account, and each line here names where the proof lives.
> "Tested" means automated checks pass against a running host on this machine, with stand-ins
> for AWS. example.com is live on a real server, and Grok deploys to it over OAuth; real mail
> in, real S3, and a real ad haven't happened yet.

| question | ready? | in one line |
| --- | --- | --- |
| 1. Host my static content | **Yes** | Markdown, HTML, templates, images. One-command deploy with TLS; the deploy offers to redirect `www.` to the site once its DNS points here. |
| 2. Dynamic content deployed from Grok | **Yes, untried with Grok** | Grok keeps a project's source in the host, then commits, builds, and deploys it there. The panel's Source view shows every revision, diff, file, and build log, and which revision each box version came from; the history is mirrored to the backup bucket as patches git can replay (`design/machine/020.Source.md` § 9 to § 12). |
| 3. GrokBot doing outreach and tracking | **Yes, untried with Grok** | `leads` and `ranches` boxes, with MCP tools an agent drives through a minted token. |
| 3a. Facebook ads to collect emails and surveys | **Yes** | A landing page, consent, UTM tags per ad, confirmation mail through SES, counts and CSV. |
| 3b. Track local ranches and their offerings | **Yes** | A CRM with an outreach log, and an offerings index the robot keeps current. |
| 3c. An agent builds a ranch's whole digital side | **Partly** | The parts are there. Payments, mail inboxes, and scheduling need outside services. |
| 4. A good robot for a local meat index | **Yes, small scale** | Polite crawling with views and extraction, built for hundreds of sites, not millions. |
| 5. Survive losing the machine | **Yes, untried with real S3** | Sealed nightlies kept 14 days, database increments every 15 minutes, attachments, and `just rehydrate`. |
| 6. Mail that arrives | **Yes, untried with real SES or a real MX** | Sign-in codes and boxes' mail out through Amazon SES; mail in through Stalwart to boxes' `routes.mail`. |
| 7. An agent people reach by email | **Yes** | Mail from the site's own address, in the person's thread, with unsubscribe and consent; model calls through the outbox with result routes. `design/machine/026.CaseStudy.md` § 3 is the recipe. |
| 8. See what the host and Grok are doing | **Yes, once its domain is set up** | The operator panel: traffic, heat, data flow, every box, the change log with diffs, and a read-only database explorer (`design/machine/028.Panel.md`). |
| 9. A site, or part of one, as an installable app | **Yes, untried on a phone** | `pwa_put`: manifest, offline worker, and push notifications from boxes (`design/machine/782.PWA.md`). |
| 10. A community that meets by mail (one digest, one reply a day) | **Yes, to build** | Markdown digests, reply-token replies cut to the person's words, calendar invites with RSVPs, attachments (`design/machine/783.ThesisCommunity.md` § 4 is the recipe). |
| 12. Invites, and mail that follows the law | **Yes, once the postal address is set** | The host invites (roles, terms, lists, accept by code, decline for good), every box mail has a category with its rule, lists need a confirmed opt-in, and a person lifts a block (`design/machine/029.Invites.md`). The next deploy asks for it. |
| 13. A blog, and deploys that check themselves | **Yes** | One `page_put` per markdown post (built-in post template, BlogPosting, OG tags); the index, feed, sitemap, and llms.txt follow. `content_deploy` writes many files and checks what visitors get (`design/machine/031.ContentKit.md`). |
| 14. Logs an auditor would accept | **Yes, archives untried with real S3** | Chained, rotated daily, kept 7 days and archived sealed for 400; an audit log; a grep-like dive; live p99 per URI; the panel's Sitemap, Performance, Agents, and Logs (`design/machine/032.Logs.md`). |
| 15. Boxes that talk to any API (Hevy, Strava, X, POSSE) | **Yes, untried against the real APIs** | Pinned OpenAPI specs, host-run OAuth 2 and pasted keys sealed in the box's database, multipart and chunked uploads, polling, rate limits (`design/machine/034.SpeakingHttp.md`). Each provider needs an app registered with `https://<domain>/goober/connect/callback`. |
| 16. Pages that update in real time | **Yes** | A strategy's writes publish a topic on commit; a `routes.sse` subscription answers again, read-only, when it moves, over SSE (`design/machine/036.Bus.md`). |
| 17. Tuning products for speed, correct by default | **Yes, caching; reports to come** | GETs change nothing, so reads run beside writes. A GET route opts into `cache`: answered without waking the box while what it read is unchanged, never shared across people, checked by sampling; `cache_status` shows hits and mismatches. MCP performance reports are next (`design/machine/037.Caching.md`). |
| 18. Ids that mean one thing, and moving data into a new database | **Yes, untried on production data** | New users, invites, uploads, and rows keyed by `id` get host ids from one service (never named by a box); old ids are kept and never repeated. Job boxes move data in batches, dry run on copies first (`design/machine/038.Ids.md`). |
| 19. Changing a table's shape | **Yes, untried on production data** | A schema's next version (`"follows"`) says what it does to each database before it's stored. Additions apply at once; drops and rewrites wait for a person in the panel and take a snapshot first; an upgrade box carries rows the host can't, dry run on copies first (`design/machine/041.SchemaEvolution.md`). |
| 11. Boxes that work with time and calendars | **Yes** | `time@1` and `ics@1` host libraries: zones, recurrence, invites, RSVPs, and proposals, tested against RFC 5545 and real clients' messages (`design/machine/831.Libraries.md`). |

## 1. Static content: yes

- **What works.**
  - Pages in markdown or HTML, Mustache layouts, listings and feeds, images, and whole HTML
    documents served as written.
  - Push a folder with `tools/goober.py push`, or keep the site as a source project
    (template `site`) and `source_deploy` it.
  - A built Astro or Next.js site works as it is.
- **Deploy.** `just deploy you@server` on Ubuntu 24.04+ or Debian 13+ sets up Caddy (TLS), a
  `goober` user, systemd, and local nightly copies. Setup asks for SES and an S3 bucket.
  Another site later: `just deploy you@server --add-tenant newsite.com` asks for just that
  tenant (owner, postal address, www.) and gives it a Caddy site of its own.
- **Watch for.** The deploy script has been dry-run here, never against a real machine.

## 2. Dynamic content from Grok: yes, untried with Grok

- **How Grok connects.** Add `https://your.domain/goober-operations-mcp` as a connector and
  choose OAuth. Grok sends you to the host operators' sign-in: your address must be in
  `identity.operators`, and the code comes by mail. Allow it, and Grok gets the operator tools
  (`design/machine/023.OperatorOAuth.md`). Clients that can send a header can instead use a
  token from `goober-host token create --name grok`.
- **The loop** (`design/human/521.Source.md`, `design/machine/020.Source.md`):
  1. `source_create` (a Rust box, a WAT box, or a site);
  2. `source_checkout`;
  3. `source_commit`;
  4. `source_deploy`.

  The host builds Rust in a sandbox with no network, so Grok needs no laptop, and the source
  stays in the machine for the next conversation. `source_revert` goes back. You can pull,
  edit, and push the same project from a terminal with `goober.py source`.
- **For Rust on the server:** every `just deploy` installs the toolchain and vendors the crates
  (serde, serde_json) on the server; `--without-rust` skips it. WAT boxes and sites need nothing.
- **Before going live.** Grok can store a version dark (`version_upload {activate: false}`),
  try its HTTP routes, tools, mail routes, and timers with `route_try` (on copies; nothing real
  changes), then `version_promote`, which warns about a version nothing tried. Timers are
  tested on a simulated clock; the real clock is checked daily with `just temporal`'s
  `temporal-daily.sh install`.
- **Proof.** `source_tools_test`, `route_try_test`. A Rust template was built, deployed, and
  serving on the demo host.
- **Watch for.**
  - The operator token is host owner: give it only to an agent you trust like yourself.
  - Every deploy goes live at once: there are no approvals yet.
  - Grok's remote MCP hasn't been tried.
  - bubblewrap on Ubuntu 24.04 is untested; it works here.

## 3. GrokBot doing outreach and tracking: yes, untried with Grok

- **A narrower key than the operator token:**
  1. `client_put` registers GrokBot as an agent.
  2. You sign in once.
  3. `agent_token_mint` gives it a token for up to a year.
  4. It calls box tools at `/mcp`.
  5. Your account page shows it and revokes it.
- **Proof.** `just products-e2e` plays GrokBot through both products, and every check passes.

**3a. Ads that collect emails and surveys: yes.**
- **The flow.**
  1. Copy `examples/site/beef-share.html`.
  2. GrokBot opens the campaign (`campaign_open`): a form posting to a campaign nobody opened
     is refused.
  3. Signups land in `leads`, and each gets a confirmation link through the host's mail
     service. It leaves only after the signup is stored, at most once a day per address.
  4. `leads_stats`, `survey_summary`, and `leads_export` show which ad works.
- **To finish.**
  - Get SES out of its sandbox (an AWS request) and verify your domain.
  - Unsubscribes and bulk campaigns aren't built: use a mailing tool for those.

**3b. Tracking local ranches: yes.** `ranches` keeps ranches, farms, and butchers. GrokBot
logs outreach with `contact_log` and moves each ranch along its statuses, and the robot reads
each ranch's site weekly.

**3c. An agent building a ranch's operations: partly.**
- **Ready:**
  - sites and forms;
  - a box for inventory, shares, and orders, kept as a source project;
  - customer lists, reminders, and dashboards;
  - mail out, and mail in: a box receives on addresses it declares (024);
  - calls to Stripe or Twilio with keys bound to their hosts.
- **Not ready:**
  - verified senders on received mail (Q39);
  - customer file uploads to boxes (060);
  - Stripe webhooks end to end;
  - a domain per ranch without a config edit;
  - approvals.

## 4. A good robot for a local meat index: yes, at small scale

- **What it does.** It obeys robots.txt, says who it is, and asks each site at most once
  every two seconds. It fetches public sites by name only and caches what it reads.
- **What it hands back.** The full HTML or simpler views (text, markdown, links, meta,
  JSON-LD, tables, `select`, `extract`). There are crawls for operators and a `web` service
  for boxes. `design/machine/017.Crawl.md`.
- **Watch for.**
  - No JavaScript rendering.
  - Hundreds of sites a week, not a global index.
  - Whether to republish what it reads is your call (Q33).

## 5. Survive losing the machine: yes, untried with real S3

- **What goes up** (`design/human/663.Backups.md`, `design/machine/019.Backups.md`):
  - every attachment, once, as it arrives;
  - a sealed `tar.gz` of everything else nightly, kept 14 days by the timestamp in its name;
  - every 15 minutes, only the database pages that changed since the night.

  Everything is encrypted before it leaves (ChaCha20 and HMAC), with a backup key that only
  you and the machine hold.
- **Getting it back.** `just rehydrate ~/copy [2026-10-26T18:00]` rebuilds any moment on a
  devbox. The devbox is a sandbox: no mail, calls, or backups leave it.
- **Proof.**
  - `s3_backup_test` against a stand-in S3.
  - `just backup-e2e`: the real binary against a Python S3 stand-in that checks AWS
    signatures its own way.
  - The signer also matches AWS's published examples and curl's own.
- **Watch for.**
  - **Copy the backup key off the machine** (`goober-host backup key`), or the backups can't
    be read.
  - Archives are at most 5 GiB.
  - The nightly needs room on disk for one more copy of storage.

## 6. Mail that arrives: yes, untried with real SES or a real MX

- **What sends.** `identity.mailer: "ses"` sends sign-in codes and every box's mail through
  SES's API with AWS keys from the key store. Boxes declare a `mail` service, and their mail
  goes through the outbox after commit, within daily limits (`design/machine/018.Mail.md`).
- **Watch for.**
  - SES starts in its sandbox: request production access and verify the domain (DKIM)
    before ads run.
  - Bounces and complaints are SES's to suppress for now.
- **What receives.** Every deploy installs Stalwart on port 25, which hands each message to the
  host; a box declares `routes.mail` addresses (`support@`, `*@`, `reply+{ticket : token}@`)
  and its answer is the SMTP answer (`design/machine/024.MailIn.md`). `just mail-e2e` runs it
  all here, with the real Stalwart. On the server: add the MX records deploy prints, and open
  inbound TCP 25 at your provider (many block it until asked). Senders aren't verified yet:
  From is only a string (Q39).

## 7. An agent people reach by email: ready to build

- **What the host gives a box:**
  - mail in on addresses it declares, with reply tokens;
  - mail out from its own address (`"mail": {"from": "hello@example.com"}`), threaded with
    `in_reply_to` and `references`;
  - a one-click unsubscribe the host enforces for every box of the tenant;
  - model calls with `effect` outbox, up to two minutes, whose answer comes back on a result
    route;
  - 20 mails a day to someone who wrote in, and 3 for outreach.
- **Proof.** `MailTest.OutreachThreadsAndUnsubscribesForGood`,
  `MailTest.RepliesInAThreadThePersonStartedGetMoreRoom`,
  `NetworkTest.ResultRoutesHearHowADeliveryWent`, and `just mail-e2e` with the real Stalwart.
- **Watch for.**
  - SES must have example.com verified as a domain, or mail from `hello@` dies.
  - Senders aren't verified (Q39), so a mail's From is only a claim.
  - Bounces and complaints don't reach the box yet.
  - The host has no spend cap on model calls: the box must count its own.

## Before real people use it

1. Deploy to the new machine (`just deploy you@server`). Answer SES and S3 in
   setup, then **store the backup key in your password manager** and run
   `goober-host backup key --confirm`.
2. In AWS:
   - verify the sending domain;
   - leave the SES sandbox;
   - create the bucket and an IAM user with SES send and S3 rights on that bucket only.
3. Run `just rehydrate` once on your laptop: a backup is real only once restored.
4. Try Grok's remote MCP against `/goober-operations-mcp` (deploying) and `/mcp` (GrokBot).
5. Decide the open questions:
   - Q24, a separate origin for the sign-in screens, before strangers sign in;
   - Q29 to Q38 in `design/QUESTIONS.md`.
6. Get an outside security review before anything holds other people's money or secrets.

## 8. See what's happening: yes, once its domain is set up

- **What works.** The operator panel, a web page of its own:
  - traffic over 1 hour to 30 days, with p50 and p99;
  - boxes as a heat map, and each box's routes, versions, logs, and manifest;
  - the data flow as a graph, from tenants to boxes to databases and services;
  - the change log, with line diffs for pages and partials, and manifest diffs for deploys;
  - every operator call (Grok's included), linked to what it changed;
  - a database explorer with a read-only SQL box.

  It only looks; changes still go through Grok's tools (`design/machine/028.Panel.md`).
- **To turn it on at example.com.** Deploy: the operations audit offers to set it up once
  `ops.example.com` resolves to the server (it does today). By hand:
  - Add an `A` record for `ops.example.com`.
  - Add the name to the site line of `/etc/caddy/Caddyfile`, then reload Caddy.
  - Set `"operations": {"domain": "ops.example.com"}` in host.json, and restart.

  Then sign in with your operator address. Locally it is `http://ops.localhost:8080/`.
- **Watch for.**
  - The change log starts when this version is deployed; earlier changes aren't in it.
  - Traffic history comes from the access logs still on disk (four files of 64 MiB).

## 9. An installable app: yes, untried on a phone

- **What works.**
  - `pwa_put` makes the whole domain (`scope: "/"`) or a part of it (`"/shop/"`) an app: the
    manifest, an offline worker, and the head tags. The built-in base template places them;
    your own layout needs `<!--#include pwa-->`.
  - Boxes send push notifications through a `push` service, to a person, a topic, or
    everyone.
- **Checked.** In headless Chromium the app installs with no errors, and pages in it work
  offline. Push is checked against the RFC's own example and a stand-in push service.
- **Watch for.**
  - Push on an iPhone works only after the app is added to the home screen.
  - Nobody has received a real push on a phone yet.

## 10. A community that meets by mail: yes, to build

- **What works.** Each member gets a daily digest at 07:00 in their zone, written in markdown
  by the agent. Their reply comes back to a reply-token address that names them and the day,
  cut to their own words (`reply_text`). The box enforces one reply a day. A model clusters
  the replies through the outbox. Past a threshold, the box sends calendar invites, with RSVPs
  back as `calendar`; files can be attached.
- **Watch for.**
  - Raise `mail.daily-max` past the member count.
  - A bounced address isn't reported to the box yet (M2).
  - The community box itself is Grok's to build from the recipe.
