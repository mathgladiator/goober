# HUMAN.md: is it working?

A one-page check for the human, while Grok runs the host. Paths assume the defaults:
the host runs as `goober` in `/srv/goober/host`, at example.com. **example.com stands for your
site's domain**: put yours in every command (the pages a deploy checks are listed in
`~/.config/goober/readiness.d/` under your domain's name, on the machine you deploy from). Run the server commands over ssh; they
need sudo.

## 1. Is it up? (from your laptop, 10 seconds)

```sh
curl -s https://example.com/goober/health        # {"ok":true,"build":"a2906b65012c"}
curl -s https://example.com/goober/ready         # storage, identity, boxes, stores, loops, mcp, queue
```

An empty 503 from Caddy (no body, `server: Caddy`) means the host isn't answering at all: go
to section 5. `/goober/ready` answers 503 with `"ok":false` and names the failing check when
the host runs but something is wrong:
- `disk` says how much room is left where storage lives. `"low": true` (under 10% or 2 GiB)
  is a warning; under 256 MiB fails the check, since every write (data, logs, backups) is
  about to. `sudo sh -c 'du -sh /srv/goober/host/storage/* /srv/goober/host/backups'`
  shows what holds it (inside `sh -c`, since only `goober` can list storage to expand the `*`); `backups/` holds a local copy of storage for each night, the newest 3 kept with S3 backups on and 14 with them off (`goober-host backup` removes older ones: only dated folders it made, with a `BACKUP.json`); `host_health`'s `limits` check (ask Grok, or the panel) has `local-copies`: how many there are and how much they hold.
- `boxes` lists any box with an active version that isn't served (`not_loaded`).
- `stores` lists any shared file that wouldn't open at start (`consent`, `web`, `source`). A
  broken web or source store was moved aside (`*.unreadable-<time>`, kept) and a fresh one
  started. A broken `consent` holds all mail until it's repaired and the host restarts, since
  nothing knows who opted out.
- `loops` names a background loop (timers, outbox, backups, ...) that stopped coming round
  or keeps failing; `journalctl -u goober-host` says why.

Then open the site in a browser. If health fails, go to section 5.

**Watch it from outside.** `/goober/ready` also answers `health`: the last five minutes' error
ratios and latency, box, mail, and service failure ratios, the outbox's depth and oldest age,
free disk, and how long since each backup. Point any uptime checker at
`https://example.com/goober/ready` (it answers 503 when something is wrong), or a Prometheus
(Grafana Cloud's free tier has one) at `https://example.com/goober/ready?format=prometheus`
and graph `goober_requests_5xx_ratio`, `goober_request_p99_milliseconds`,
`goober_outbox_oldest_age_seconds`, `goober_disk_free_ratio`, and
`goober_backup_nightly_age_seconds`. Alert on `goober_ready == 0`. It shows no request counts,
since anyone can read it.

Which code is running: `build` is the git commit the host was built from (`+dirty` if the
tree had uncommitted changes). It should match `git log -1 --format=%h --abbrev=12` on the
machine you deployed from. `host_health` (ask Grok) and `goober-host --version` on the
server say the same.

## 2. What has Grok done? (on the server)

Easiest: open the panel, https://ops.example.com/ (once it is set up: READINESS.md § 8).
Changes shows every deploy, page, partial, and schema write, with diffs and who made it, and
Changes → actions shows every tool Grok called. Without the panel:

Every operator tool call is one line in the access log: when, who, which tool, which tenant,
and whether it failed.

```sh
sudo python3 -c '
import json
for line in open("/srv/goober/host/storage/logs/access.log").readlines()[-5000:]:
    e = json.loads(line)
    if e.get("operations") == "mcp" and "tool" in e:
        print(e["at"][:19], e["operator"][:40], e["tool"], e.get("arg_tenant", ""), e.get("arg_path", e.get("arg_name", "")),
              "FAILED" if e.get("tool_error") else "")
' | tail -n 40
```

`oauth:you@…:dyn_…` is Grok, signed in as you. Lots of `FAILED` lines means Grok is stuck;
tell it what the error says, or ask it to read `box_logs`.

## 3. Is anything broken?

```sh
sudo grep -c '"status":5' /srv/goober/host/storage/logs/access.log   # server errors today (UTC); earlier ones are in access.log.1, .2, ... (7 days)
sudo grep '"status":5' /srv/goober/host/storage/logs/access.log | tail -n 5
sudo journalctl -u goober-host --since today | grep -iE 'error|failed' | tail -n 20
```

A few 5xx from a box Grok just deployed is normal while it iterates. Steady 5xx on a page
people use is not.

**The panel says more:** **Performance** (each URI's p99, live), **Sitemap** (the whole site,
colored by p99), **Agents** (who called what; red flags when an agent goes ham), and **Logs**
(search the last 7 days, newest first; click a line to follow its request id). Logs keep
addresses only as hashes: type an address in the Logs view's address box to find its lines.
Every log line is chained to the one before; to check nothing was edited or deleted, ask Grok
for `log_verify`, or see `log_status` for what's waiting to be archived. Rotated logs go to the
backup bucket under `logs/`, sealed, for 400 days.

**Do live updates reach browsers?** Once a box has a `routes.sse` subscription (036), this should
print an `event: update` at once and a `: ping` every 15 seconds, not hang silent:

```sh
curl -sN -H 'Accept: text/event-stream' https://example.com/<the subscription's path> | head -n 8
```

**Is the kernel patched, and is multipath TCP off?** Deploys turn MPTCP off (the kernel's MPTCP
code has had remotely reachable flaws), but only a newer kernel fixes the kernel:

```sh
sysctl net.mptcp.enabled        # want: net.mptcp.enabled = 0
sudo ss -Mln | tail -n +2       # MPTCP listeners; want: nothing
uname -r; sudo apt list --upgradable 2>/dev/null | grep -i linux-image   # then apt upgrade and reboot
```

## 4. Are backups and mail fine?

```sh
sudo -u goober goober-host backup list --config /srv/goober/host/host.json   # a nightly within the last day
sudo journalctl -u stalwart -n 20                                           # mail in (port 25)
sudo tail -n 5 /srv/goober/host/storage/logs/mail.log                       # mail out
```

Or ask Grok to run `host_health`, which reports backups, how mail leaves, and startup notes,
and `mail_activity` with the tenant (example), which lists the site's mail in and out and
what happened to each (a `mail_template_send_test` shows as `test`). `mail_domain_check` with
example.com checks the mail DNS (MX, SPF, DMARC, SES's DKIM) and says which records to add;
`host_health`'s `smtp_listener` says whether Stalwart answers on port 25.

**Do timers keep real time?** The tests fake the clock, so a daily run checks the real one, on
the machine that holds this repo (your laptop or a dev box, not the server). Once:
`host-src/tools/temporal-daily.sh install`. Then each day's verdict is the newest line of
`temporal-logs/summary.jsonl` (`"pass": true`); a failing day's `temporal-*.jsonl` names each
late, missed, or doubled fire. Run one by hand with `just temporal`; remove the daily run with
`host-src/tools/temporal-daily.sh uninstall`.
**Mail needs your postal address.** Outreach, lists, and invites are refused until the host
config names it (CAN-SPAM). `host_health` lists `tenants_without_postal_address`. On the server,
edit `/srv/goober/host/host.json` and add, next to `domains`:

```json
"tenants": {"example": {"postal-address": "Example Co, 123 Main St, Springfield, IL 62701",
                         "terms": "/terms", "privacy": "/privacy",
                         "invites": {"roles": ["member"]}}}
```

then `sudo systemctl reload goober-host`. `terms` and `privacy` are pages on the site; with
them, signing in and accepting an invite say "by continuing, you accept" and record it.
`invites.roles` is what a box's invites may grant. Someone who declined or unsubscribed stays
blocked until they allow mail on their account page, or you lift it in the panel
(**Operators**, **Lift a mail block**, saying why). Grok can't lift one.

**Connecting people's accounts (Strava, X, Hevy).** Boxes keep people's keys and tokens sealed
by the host. Copy the vault key beside the backup key:
`sudo -u goober goober-host vault key --config /srv/goober/host/host.json` (add `--create` to make
it now, before anyone has connected an account; an existing key is never replaced). Without it,
everyone must connect again. For an OAuth provider (Strava, X), register an app with the redirect URI
`https://example.com/goober/connect/callback`, then have Grok `key_set` its client id and secret
(hosts: the provider's token host).

Is the backup key copied somewhere safe? Without it no backup can be read. Print it with
`sudo -u goober goober-host backup key --config /srv/goober/host/host.json`, keep it somewhere
other than the server, then run the same with `--confirm`. Only you can do this: `host_health`
keeps saying nobody has until you do.

**Backups say "cannot read … (owner root, mode …)" or "Permission denied".** Something in
storage isn't the host's user's, usually from a `goober-host` command run with `sudo` but
without `-u goober`. The error names the path. To see every such path:

```sh
sudo find /srv/goober/host/storage ! -user goober                  # made by someone else
sudo -u goober find /srv/goober/host/storage ! -readable           # the host can't read
sudo -u goober goober-host audit --config /srv/goober/host/host.json   # the storage check
```

A deploy gives them back to `goober` and says which it changed. By hand:
`sudo chown -hR goober: /srv/goober/host/storage`. Run every `goober-host` command on the
server as `sudo -u goober`. Folders backups never hold (`tmp/`, `logs/`, build caches) no
longer stop a backup either way.

## 5. Fix it by deploying again

```sh
just deploy example.com --user customer
```

It upgrades everything and ends with `── checking everything this host needs`. The last line
must be `✓ every check passed`. Any `✗` line says what is wrong.

Before that last line, `── operations audit` says how the host is run: `✓` fine, `!` worth
fixing, `?` a suggestion with its steps, `·` one you silenced. It never fails the deploy. It
offers to set up the panel at ops.example.com (answer `y`), or `v` to never ask again, and
then to redirect www.example.com to example.com the same way (once www has a DNS record
pointing at the server; `v` never asks again about that name). Then
`── settings` asks for anything missing (the postal address, the mail's from address, the
backup bucket, AWS keys, an operator): `y` and type each value (Enter skips one), `n` for
next time, `v` never. If the host won't start with them, host.json is put back. To
silence any check, add its id to host.json: `"audit": {"quiet": ["backup"]}`. The same
audit any time: `sudo -u goober goober-host audit --config /srv/goober/host/host.json
--caddyfile /etc/caddy/Caddyfile`.

**Launch another site** on the same server, with its own tenant (accounts, data, pages):

```sh
just deploy example.com --user customer --add-tenant newsite.com
```

`── a new tenant` asks for just that site: its tenant name (Enter takes the one shown), its
first owner's email, its postal address, its terms and privacy pages (Enter skips each), and
whether to redirect www.newsite.com too. It prints the DNS records to add (an A record to this
server; MX if mail comes in) and whether they're there yet, then asks before changing anything.
`y` adds it to host.json and gives it a Caddy site of its own; if the host or Caddy refuses,
both files are put back. Before DNS points here the site is added but has no certificate yet;
deploy again once it does, or just wait: Caddy gets one on the first visit.

Stalwart (mail in) is left running as it is unless something it's made from changed (its
version, its configuration, the domains it relays for) or it isn't up; the deploy says which.
`--reprovision-mail` on the deploy provisions it anyway.

If any step fails partway, the deploy says which line, starts Stalwart again if it had
stopped it, and puts the previous build back unless the new one is up and ready. A new build
that isn't ready within a minute is rolled back on its own: the deploy prints the
host's last 60 log lines (the reason), puts the previous build back, and fails. Then the
server checks itself through its public name, as people and agents reach it
(`~/.config/goober/readiness.d/example.com` lists the pages), and publishes the result at
https://example.com/goober/deploy, summed up in `/goober/ready` as `deploy`. While the host
restarts, Caddy holds requests up to 15 seconds; if it's down, Caddy answers a JSON 503 with
Retry-After, so clients retry rather than remember a failure. The same check by hand, with an
operator token for the boxes:

```sh
GOOBER_TOKEN=... python3 host-src/tools/readiness.py https://example.com \
  $(grep -v '^#' ~/.config/goober/readiness.d/example.com)
```

To check signing in and refreshing for real (what Grok's and Cursor's connectors do), keep one
client for it: a file holding {"client_id", "refresh_token"}, which each run refreshes and
rewrites. Add `--refresh-file ~/.goober/example-refresh.json` to the command above.

If the host is down and you want its reason now:

```sh
ssh customer@example.com 'sudo journalctl -u goober-host -n 60 --no-pager'
ssh customer@example.com 'sudo -u goober goober-host check --config /srv/goober/host/host.json'
# Back to the build before the last deploy:
ssh customer@example.com 'sudo cp -p /usr/local/bin/goober-host.prev /usr/local/bin/goober-host && sudo systemctl restart goober-host'
```

## 6. Stop Grok

**Gentlest: make Grok ask first.** In the panel's **Operators** view, the **Approvals** card
lists each connected agent. Turn approvals on for Grok, and every change it tries waits there
for you to approve or deny; it can still look around. Turn it off and it acts alone again.

From least to most drastic:

```sh
# End every login and OAuth grant your operator address holds (Grok's included).
sudo -u goober goober-host operator revoke you@example.com --config /srv/goober/host/host.json
# Any minted operator tokens: list them, then revoke by name.
sudo -u goober goober-host token list --config /srv/goober/host/host.json
sudo -u goober goober-host token revoke grok --config /srv/goober/host/host.json
# Take the whole host down (the site too).
sudo systemctl stop goober-host
```

To let Grok back in, connect it again; it signs you in by email code.

**Give each engineer and agent its own token**, so the logs say who did what. The change log,
the access log, and the panel's agent audit name every call by its token: `token:grok-data` for
a minted one, `oauth:you@example.com:<client>` for an OAuth sign-in, and `config` for the
`operations.token` in host.json, which everyone holding it shares. Mint one per agent, and
revoke it by name when that agent is done:

```sh
sudo -u goober goober-host token create --name grok-data --config /srv/goober/host/host.json
```

An agent asks `operator_scope` to see the name it is recorded under; one holding the shared
token is told to ask you for its own.

**Keep Grok on one site** without stopping it: in the panel, **Operators**, uncheck "all" on
your row and check the one tenant. Every agent signed in as you is then held to it, and so are
you in the panel, until you check "all" again. A minted token can start that way:
`goober-host token create --name grok --tenants example`. Grok can ask `operator_scope`
where it stands, but no tool can change it.

**Grok doesn't see a tool that exists** (after a deploy added it): Grok keeps the tool list
it fetched when it connected. Disconnect and reconnect the connector; then ask it to list
the tools again. Every `tools/list` a site's `/mcp` answers is a line in the access log
(`"mcp":"tools/list"`) naming the tools it listed and their box versions:

```sh
sudo grep '"mcp":"tools/list"' /srv/goober/host/storage/logs/access.log | tail -n 5
```

No line after a reconnect means the client didn't ask; an old list there means the host
served it. For a site's own `/mcp` (box tools), the operator tool `mcp_tools_preview`
with the domain and a person's email shows what that person's list is right now, which box
version each tool comes from, and why any is hidden. `route_check` with a domain and a path
says which box, page, or host path answers it.

**Allow does nothing when an app (Cursor, Grok's apps) connects:** see what the host did:

```sh
sudo grep -E '"oauth":"(authorize_[a-z_]*|register_refused|consent)"' /srv/goober/host/storage/logs/access.log | tail -n 5
```

`authorize_allowed` means the host answered; `"to"` names where it sent the browser. If
the app didn't open, press "Open …" on that page. `authorize_cross_origin` lists what the
browser sent; `register_refused` means the app's redirect wasn't accepted.

**Grok's connectors drop and ask to sign in again:** look for refused refreshes:

```sh
sudo grep '"grant":"refresh_token"' /srv/goober/host/storage/logs/access.log | grep '"error"' | tail -n 5
```

`"why"` says the reason: a spent token used more than a minute after it was replaced revokes
the grant (two parties holding one grant), and Grok must sign in again.

## 7. Undo something Grok broke

- **A box:** every upload is a version. Ask Grok to `version_list` the box and
  `version_promote` the one before; or do it yourself with a token
  (`goober-host token create --name me`, then from the repo:
  `GOOBER_URL=https://example.com GOOBER_TOKEN=… host-src/tools/goober.py call version_promote box=NAME version=ID`).
  The 3 versions that were live most recently are always there. An older one is archived 30
  days after it stopped being live: the promote answers that, and
  `host-src/tools/goober.py call version_restore box=NAME version=ID why="roll back"` brings it
  back first. After 400 days it is deleted; `version_list` still names it, who uploaded it, and
  when it was live, but it can't come back.
- **Source:** `source_log` and `source_revert` on the project. The panel's **Source** view shows
  every revision's diff, its files, and each build's log; a box's page says which revision
  each version came from.

**Read the source history from the bucket** (every revision, sealed, kept for good), on your
laptop with the same keys as `just rehydrate`:

```sh
just source-mirror ~/goober-source   # reads ~/.config/goober/backup.env; add a project to fetch one
git init ~/greeter-history && cd ~/greeter-history && git am ~/goober-source/greeter/patches/*.patch
git log --stat                       # every revision, who made it, and why
```

A deployed revision's files, with the box versions and build log, are in
`~/goober-source/PROJECT/snapshots/NNNNNN.tar.gz` (`.goober-deploy.json` inside).
- **Pages, data, everything:** restore a backup onto your laptop to look first
  (`just rehydrate ~/copy`), then decide.

## Clean up: drop old boxes and databases

Open the panel's **Cleanup** view (`design/machine/051.Cleanup.md`). Signed in as host:owner:

- **Grok asks, you decide.** Tell Grok "ask to drop the boxes the migration replaced, and their
  databases". It runs `box_drop` and `db_drop`; nothing goes until you press **Approve…** under
  "Waiting for you" and type the name. Each request shows what it takes and what blocks it.
- **Or drop it yourself.** Every box and database is listed; press **Drop…**, type its name,
  and say why.
- **Order matters.** A database is refused while a box uses it, and a box is refused while
  another box calls it: drop the box first, then its database (it shows as bound by no box).
- **What goes.** A box: out of service, every version (tombstones say what each was), its flags.
  Its databases stay until their own drop. A database: snapshotted first (the answer names the
  snapshot; `db_snapshots` lists them).
- **Undo.** A database: `db_restore_to_new` from its snapshot. A box: deploy it again from its
  source project, or rehydrate a backup from before the drop.

Unbound tables, schemas, and snapshots are still in the **Database** view, under "Unbound, and
snapshots".

## Fetch an export Grok made

When Grok needs private rows to reach you (a list of emails, say), it runs `db_export` and
gives you an id; the rows never pass through Grok. From your laptop, straight into a file
there (`--out -` writes the CSV to standard output, so nothing unsealed stays on the server):

```sh
ssh customer@example.com 'sudo -u goober goober-host export list --config /srv/goober/host/host.json'
ssh customer@example.com 'sudo -u goober goober-host export get ID --out - --config /srv/goober/host/host.json' > rows.csv
```

Exports go away after 7 days, or now with `export delete ID`.

## Something is "near its limit"

The host keeps a budget for everything it grows on its disk: tries' copies, exports, the
panel's traffic numbers, snapshots, logs, and temporary files (`design/machine/045.Limits.md`).
See how full each one is:

```sh
curl -s https://example.com/goober/ready | jq '.checks.limits'
```

Each one is a `share` of its limit (0.85 is 85%), and `near` lists anything at 80% or more. The
page is public, so it shows shares only; the bytes and counts behind them are in `host_health`
(ask Grok, or the panel's Overview). The shares are measured at most every 30 seconds. Usually
the host handles it itself: it drops old traffic
numbers, and it refuses a try or an export that wouldn't fit, saying which limit. Snapshots are
yours: delete old ones in the panel's **Database** view. To raise a limit, set it in host.json
and reload:

```json
"limits": {"try-bytes": 1073741824, "snapshot-files": 200}
```

```sh
sudo systemctl reload goober-host
```

**A log is growing fast.** `near` says "access grew 6 times its usual day", or that at today's
rate the logs would reach some share of `limits.logs-bytes` within 7 days (`host_health` says it
in MiB: "access wrote 40 MiB in the last 24 hours, 6 times its usual 7 MiB a day"). Open the
panel's **Logs and uploads** view (or have Grok run `log_velocity`). Its "Who writes the most"
table names the tenant, box, and route behind it. Then:
- **A box logging too much:** Grok ships a version that logs less.
- **A burst of real visitors:** nothing to do; it passes.
- **A scanner hammering one path:** block it at Caddy.

The warning clears once a day's volume falls back under three times the usual. If the logs are
just bigger now, raise `"logs-bytes"` in `limits`.

**Uploads past the daily budget.** Backups count what they put in the bucket each UTC day.
Past `limits.upload-bytes-per-day` (1 GiB unless set), log archives, increments, and the source
mirror wait for the next day. The nightly and attachments always go, and a log archive waiting
a week goes anyway, so nothing is lost. `near` warns from 80%. The **Logs and uploads** view
shows each day by stream. If the budget is just too small for your site, raise it in `limits`:

```json
"limits": {"upload-bytes-per-day": 4294967296}
```

`0` turns the budget off. To upload less, keep fewer days (`backup.keep-days` only changes
what the bucket holds), back up databases less often (`backup.databases-every-ms`), or log less.

## A site is being hammered (controls)

Every domain can have its own limits: a rate, how many requests at once, body size, a timeout,
or closed. They can be set for the whole domain, one API (a box, `content`, `mcp`, `identity`),
or one endpoint like `POST /cart/{id}` (`design/machine/047.Controls.md`). See how much they
refuse across the host:

```sh
curl -s https://example.com/goober/ready | jq '.health.controls'
```

The panel's **Controls** view shows each one, and what it refused or would have refused. Ask
Grok to:

1. Add the limit in observe, so it refuses nothing yet: `control_set` with `"mode": "observe"`
   and a why.
2. Read `control_report` after a while. "Would have refused 2% from 1 client" is a flood to
   shield against. "From 40 clients" means the limit is too tight.
3. Then enforce it: `control_set` with `"mode": "enforce"`.

To close a domain or a box at once (moving data, an incident), use `"set": {"closed":
{"retry-after-s": 600, "message": "Back soon."}}` with `"mode": "enforce"`. Visitors get a 503
with your message, and no box wakes. `control_remove` opens it again. Every change is in the
change log, and in the audit log with its why. Visitors refused by a rate or concurrency limit
read only "rate limit reached", never which limit.

**A control is bum: override it yourself.** You don't need Grok, and nothing waits for
approval. Sign in to the panel as `host:owner` or `host:operator` and open **Controls**, then
pick the domain:

- **Switch to observe** stops a control refusing anything at once, while still counting what it
  would refuse. **Enforce** turns it back on. **Remove** takes it away.
- **Edit…** changes its properties (JSON like `{"rate": {"per-minute": 600}}`) or its match.
- **Add a control** makes an override for one endpoint: an id, a match like `POST /cart/{id}`
  (or `TOOL /search` for an agent tool), the properties, and observe or enforce.
- A box's own limits (from its manifest, shown as `box:<box>/<route>`) can't be edited, but
  **Override…** makes an override that beats them at once.

Each needs a why. The audit log says `panel.control_set` or `panel.control_remove` with your
address, and the change log has it under `control`:

```sh
sudo grep '"panel.control_' /srv/goober/host/storage/logs/audit.log | tail -n 5
```

Agents, Grok included, get a strict rate and a fixed queue on MCP (`/mcp` and the operator
endpoint), whatever controls say: they can front-load calls freely, and calls past two at once
wait their turn instead of failing. "rate limit reached" means slow down. See how often it
happens:

```sh
curl -s https://example.com/goober/ready | jq '.health.mcp'
```

`refused_ratio` near 0 is right; a high `waited_ratio` means agents are front-loading, which is
fine. The Controls view shows each queue, and `mcp_queue_report` explains it
(`design/machine/047.Controls.md` § 13).

## Try a new version on part of the traffic (splits)

A split sends a share of a box's real traffic to a new version beside the live one
(`design/machine/052.Splits.md`). Each person stays on one version.

1. **Store the new version dark.** Grok runs `version_upload` with `activate: false`.
2. **Start the split.** `split_start {box, version, why}` climbs 1%, 5%, 25%, 50%, 100%, a step
   every 30 minutes once the new version has served 200 requests in it. At 100% the new version
   becomes the live one. `share: 5` holds 5% instead, until you say otherwise.
3. **Watch it.** The panel's page for the box shows both versions side by side: requests, 5xx,
   p50, p99. If the new version's errors or p99 get worse than the live one's past the
   guardrails, the host rolls back on its own.
4. **Decide.** `split_stop {box, outcome: "promote"}` ships it now; `"rollback"` sends everyone
   back to the live version at once.

## Where is a site's heat?

A heat study counts one domain's requests by endpoint for a while, then keeps a report 30 days
(`design/machine/048.HeatStudy.md`). It changes nothing that is served. Use one before setting
rate or concurrency limits, or when the site feels slow:

1. **Start it for an hour at a busy time.** Have Grok run
   `heat_study_start {domain: "example.com", minutes: 60, why: "Monday's peak"}`. One runs per
   domain, and four at most on the host.
2. **Read it.** Open the panel's **Studies** view, or have Grok run `heat_study_get {id}`.
   "Where the heat is" shows which endpoints take the time; the endpoints table shows how hard
   the busiest clients push each one (client p99 per minute) and how many run at once. The
   host's own paths (`/goober/*`) aren't counted.
3. **Send the suggestions in observe mode.** Each endpoint's suggested controls are ready
   `control_set` arguments with `mode: "observe"` and the reason for each number. Have Grok send
   the ones you agree with as they are.
4. **Check `control_report`** after a day: what each control would have refused, and whom.
5. **Enforce** the ones that would have refused only what they should: the same `control_set`
   with `mode: "enforce"`.

To end a study early, `heat_study_stop {id}`; what it gathered stays. A restart ends a running
study too, keeping what it had saved (at most a minute old).

## What does the team know? (skills)

Grok's engineers write down what they learn as skills (how to do one thing) and playbooks (what
to do when something happens), each for one or more roles (`design/machine/050.Skills.md`).

- **Read them.** Open the panel's **Skills** view: search, or click a role. Each skill shows who
  wrote it, its revision, and every change.
- **Ask for one.** Tell Grok "write a skill for box engineers on how you deploy", and it saves it
  with `skill_put`. A new agent starts with `skill_list {role: "box-engineer"}`.
- **A bad one.** Have Grok fix it with `skill_put`, or remove it with `skill_delete`; the change
  log keeps the old text either way (the panel's **Changes** view, kind `skill`).

## Someone asks to be deleted

People can delete their own account on the site's account page (/identity/account). If someone
asks you instead, have Grok run `user_delete` with their address and why. The panel's
**Operators** view lists deletions that a box hasn't erased yet; past 30 days they show in red.

## When in doubt

`READINESS.md` says what is built and what isn't; `design/QUESTIONS.md` lists the decisions
waiting on you.
