# HUMAN.md: is it working?

A one-page check for the human, while Grok runs the host. Paths assume the defaults:
the host runs as `goober` in `/srv/goober/host`, at kcmeatup.com. Run the server commands
over ssh; they need sudo.

## 1. Is it up? (from your laptop, 10 seconds)

```sh
curl -s https://kcmeatup.com/goober/health        # {"ok":true,"build":"a2906b65012c"}
curl -s https://kcmeatup.com/goober/ready         # every subsystem: storage, identity, boxes, mcp, queue
```

An empty 503 from Caddy (no body, `server: Caddy`) means the host isn't answering at all: go
to section 5. `/goober/ready` answers 503 with `"ok":false` and names the failing check when
the host runs but something is wrong.

Then open the site in a browser. If health fails, go to section 5.

Which code is running: `build` is the git commit the host was built from (`+dirty` if the
tree had uncommitted changes). It should match `git log -1 --format=%h --abbrev=12` on the
machine you deployed from. `host_health` (ask Grok) and `goober-host --version` on the
server say the same.

## 2. What has Grok done? (on the server)

Easiest: open the panel, https://ops.kcmeatup.com/ (once it is set up: READINESS.md § 8).
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
sudo grep -c '"status":5' /srv/goober/host/storage/logs/access.log   # server errors, all time
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

## 4. Are backups and mail fine?

```sh
sudo -u goober goober-host backup list --config /srv/goober/host/host.json   # a nightly within the last day
sudo journalctl -u stalwart -n 20                                           # mail in (port 25)
sudo tail -n 5 /srv/goober/host/storage/logs/mail.log                       # mail out
```

Or ask Grok to run `host_health`, which reports backups, how mail leaves, and startup notes,
and `mail_activity` with the tenant (kcmeatup), which lists the site's mail in and out and
what happened to each.
**Mail needs your postal address.** Outreach, lists, and invites are refused until the host
config names it (CAN-SPAM). `host_health` lists `tenants_without_postal_address`. On the server,
edit `/srv/goober/host/host.json` and add, next to `domains`:

```json
"tenants": {"kcmeatup": {"postal-address": "KC Meat Up, 123 Main St, Kansas City, MO 64105",
                         "terms": "/terms", "privacy": "/privacy",
                         "invites": {"roles": ["member"]}}}
```

then `sudo systemctl reload goober-host`. `terms` and `privacy` are pages on the site; with
them, signing in and accepting an invite say "by continuing, you accept" and record it.
`invites.roles` is what a box's invites may grant. Someone who declined or unsubscribed stays
blocked until they allow mail on their account page, or you lift it in the panel
(**Operators**, **Lift a mail block**, saying why). Grok can't lift one.

Is the backup key copied somewhere safe? Without it no backup can be read
(`goober-host backup key --confirm` once you've copied it).

## 5. Fix it by deploying again

```sh
just deploy kcmeatup.com --user customer
```

It upgrades everything and ends with `── checking everything this host needs`. The last line
must be `✓ every check passed`. Any `✗` line says what is wrong.

A new build that isn't ready within a minute is rolled back on its own: the deploy prints the
host's last 60 log lines (the reason), puts the previous build back, and fails. Then the
server checks itself through its public name, as people and agents reach it
(`host-src/tools/readiness.d/kcmeatup.com` lists the pages), and publishes the result at
https://kcmeatup.com/goober/deploy, summed up in `/goober/ready` as `deploy`. While the host
restarts, Caddy holds requests up to 15 seconds; if it's down, Caddy answers a JSON 503 with
Retry-After, so clients retry rather than remember a failure. The same check by hand, with an
operator token for the boxes:

```sh
GOOBER_TOKEN=... python3 host-src/tools/readiness.py https://kcmeatup.com \
  $(grep -v '^#' host-src/tools/readiness.d/kcmeatup.com)
```

To check signing in and refreshing for real (what Grok's and Cursor's connectors do), keep one
client for it: a file holding {"client_id", "refresh_token"}, which each run refreshes and
rewrites. Add `--refresh-file ~/.goober/kcmeatup-refresh.json` to the command above.

If the host is down and you want its reason now:

```sh
ssh customer@kcmeatup.com 'sudo journalctl -u goober-host -n 60 --no-pager'
ssh customer@kcmeatup.com 'sudo -u goober goober-host check --config /srv/goober/host/host.json'
# Back to the build before the last deploy:
ssh customer@kcmeatup.com 'sudo cp -p /usr/local/bin/goober-host.prev /usr/local/bin/goober-host && sudo systemctl restart goober-host'
```

## 6. Stop Grok

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

**Keep Grok on one site** without stopping it: in the panel, **Operators**, uncheck "all" on
your row and check the one tenant. Every agent signed in as you is then held to it, and so are
you in the panel, until you check "all" again. A minted token can start that way:
`goober-host token create --name grok --tenants kcmeatup`. Grok can ask `operator_scope`
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
  `GOOBER_URL=https://kcmeatup.com GOOBER_TOKEN=… host-src/tools/goober.py call version_promote box=NAME version=ID`).
- **Source:** `source_log` and `source_revert` on the project.
- **Pages, data, everything:** restore a backup onto your laptop to look first
  (`just rehydrate ~/copy`), then decide.

## When in doubt

`READINESS.md` says what is built and what isn't; `design/QUESTIONS.md` lists the decisions
waiting on you.
