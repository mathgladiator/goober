# Readiness

> How ready goober is for the human's experiments, as of 2026-10-27. Short on purpose:
> `PROGRESS.md` has the full account, and each line here names where the proof lives.
> "Tested" means automated checks pass against a running host on this machine. Nothing here
> has met a real server, a real ad, a real mail provider, or Grok yet.

| question | ready? | in one line |
| --- | --- | --- |
| 1. Host my static content | **Yes** | Markdown, HTML, templates, images; one-command deploy with TLS and nightly backups. |
| 2. Dynamic content deployed from Grok | **Yes, untried with Grok** | Grok holds an operator token and calls `version_upload`; boxes in WAT need no toolchain. |
| 3. GrokBot doing outreach and tracking | **Yes, untried with Grok** | `leads` and `ranches` boxes, with MCP tools an agent drives through a minted token. |
| 3a. Facebook ads to collect emails and surveys | **Yes**, add a mail provider | A landing page, a form, consent, UTM tags per ad, counts and CSV for the agent. |
| 3b. Track local ranches and their offerings | **Yes** | A CRM with an outreach log, and an offerings index the robot keeps current. |
| 3c. An agent builds a ranch's whole digital side | **Partly** | The parts are there; payments, mail inboxes, and scheduling need outside services. |
| 4. A good robot for a local meat index | **Yes, small scale** | Polite crawling with views and extraction; built for hundreds of sites, not millions. |

## 1. Static content: yes

- **What works.** Pages in markdown or HTML, Mustache layouts, listings and feeds, images as
  attachments, and whole HTML documents served as written.
  - Push a folder with `tools/goober.py push <tenant> <dir>`. Big pushes retry on 429, and
    operators may send 64 MiB.
  - A built Astro or Next.js site works as it is (`_astro/`, `_next/` are served).
- **Deploy.** Run `just deploy you@server` on Ubuntu 24.04+ or Debian 13+. It sets up:
  - Caddy, with automatic TLS and compression;
  - a `goober` system user and the systemd unit;
  - nightly backups, kept two weeks.
  - Run it again to upgrade (`host-src/README.md` § Deploy).
- **Watch for.**
  - Backups sit on the same disk: copy them off yourself.
  - Sign-in mail needs SMTP set up in setup.
  - The deploy script has been dry-run here, never against a real machine.

## 2. Dynamic content from Grok: yes, untried with Grok

- **How Grok connects.** Mint an operator token on the server
  (`goober-host token create --name grok`) and give Grok
  `https://your.domain/goober-operations-mcp` with `Authorization: Bearer <token>`.
- **What Grok does first.** It reads `box_guide`, writes a manifest and a box, and deploys
  with `version_upload`:
  - A box in WAT text needs no toolchain.
  - A box in Rust is built wherever cargo is; `rust_binding` hands over the binding.
- **Watch for.**
  - The operator token is host owner: it can do anything. Give it only to an agent you trust
    like yourself.
  - Every upload goes live at once; there are no approvals yet.
  - Grok's remote MCP tool has not been tried against this endpoint.

## 3. GrokBot doing outreach and tracking: yes, untried with Grok

- **Give it a narrower key than the operator token.**
  1. `client_put` registers GrokBot as an agent.
  2. You sign in once.
  3. `agent_token_mint` gives it a token for up to a year, acting for you. Its reach is set
     by your roles and by which routes allow agents.
  4. It calls box tools at `https://your.domain/mcp`.
  5. You see what it did, and can revoke it, on your account page.
- **Proof.** `just products-e2e` plays GrokBot through both products: 30 checks, all passing.

**3a. Ads that collect emails and surveys: yes.**
- **The flow.**
  1. Copy `examples/site/beef-share.html`: a landing page with consent, survey questions,
     the ad's UTM tags, and a honeypot.
  2. It posts to the `leads` box, which keeps one row per email per campaign.
  3. GrokBot asks `leads_stats` which ad works, `survey_summary` for the tallies, and
     `leads_export` for a CSV.
  - Inline scripts and the Facebook pixel work on whole HTML pages.
- **To finish.**
  - Turn on confirmation mail: a Resend key and the snippet in `examples/boxes/leads/README.md`.
  - Unsubscribes and sending campaigns are not built. Use a mailing tool for bulk mail.

**3b. Tracking local ranches: yes.**
- The `ranches` box keeps ranches, farms, and butchers in one shared index.
- GrokBot adds them, logs every email, call, and visit with `contact_log` (so nobody is
  contacted twice), and moves each ranch along prospect, contacted, replied, interested,
  partner.
- The robot reads each ranch's site weekly for what it sells.
- Finding ranches in the first place is GrokBot's job: search, maps, market lists.

**3c. An agent building a ranch's operations: partly.**
- **Ready.**
  - A ranch's site and order or waitlist forms.
  - A box for inventory, shares, and orders.
  - Its customer list, reminders by timer, and dashboards on the dumb terminal.
  - Calls to outside services with keys the box never sees: Stripe, Resend, Twilio, through
    the outbox.
- **Not ready.**
  - Receiving mail (038).
  - File uploads from customers (060).
  - Payments end to end: Stripe is reachable, but its webhooks need a route written for them.
  - Each ranch on its own domain and sign-in: there is no domain registry yet, so it's a
    config edit and a reload.
  - Approvals before an agent's code goes live.

## 4. A good robot for a local meat index: yes, at small scale

- **What the robot does.** It obeys robots.txt, says who it is, and asks each site at most
  once every two seconds. It reaches only public addresses and caches pages, refetching
  conditionally.
- **What it hands back.** The full HTML, or views: text, markdown, links, meta (with the
  shop platform, like Shopify), schema.org JSON-LD (Product, Offer, LocalBusiness), tables,
  CSS `select`, and `extract`, a three-selector scraper.
- **Who uses it.**
  - Operators: `web_fetch` and background crawls (`crawl_start`, `crawl_records`).
  - Boxes: a `web` service.
- **The `ranches` box reads**
  - schema.org data;
  - a Shopify store's `/products.json`;
  - or a per-ranch extract spec.
- **Proof.** `design/machine/017.Crawl.md`, and the web tests plus `products-e2e`.
- **Watch for.**
  - No JavaScript rendering: sites that draw their prices with script need their JSON
    endpoint or a manual `offering_add`.
  - One machine at two seconds per site handles hundreds of sites a week, not a global index.
  - Whether to republish what it reads is your call (QUESTIONS.md Q33).
- **The design.** `design/human/800.Crawl.md` is yours to edit; I drafted it and left it
  uncommitted.

## Before real people use it

1. Deploy to the new machine and set up SMTP or a mail API.
2. Try Grok's remote MCP against `/mcp` and `/goober-operations-mcp`.
3. Decide Q24, a separate origin for the sign-in screens, before strangers sign in.
4. Decide Q29 to Q33: shared tenants, minted tokens, and what the robot may publish.
5. Get an outside security review before anything holds other people's money or secrets.
