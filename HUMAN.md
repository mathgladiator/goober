# HUMAN.md: is it working?

A one-page check for the human, while Grok runs the host. Paths assume the defaults:
the host runs as `goober` in `/srv/goober/host`, at kcmeatup.com. Run the server commands
over ssh; they need sudo.

## 1. Is it up? (from your laptop, 10 seconds)

```sh
curl -s https://kcmeatup.com/goober/health        # {"ok":true}
```

Then open the site in a browser. If health fails, go to section 5.

Which code is running: `host_health` (ask Grok) reports `build`, the git commit, and on the
server `goober-host --version` prints it too. It should match `git log -1 --oneline` on the
machine you deployed from.

## 2. What has Grok done? (on the server)

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

## 4. Are backups and mail fine?

```sh
sudo -u goober goober-host backup list --config /srv/goober/host/host.json   # a nightly within the last day
sudo journalctl -u stalwart -n 20                                           # mail in (port 25)
sudo tail -n 5 /srv/goober/host/storage/logs/mail.log                       # mail out
```

Or ask Grok to run `host_health`, which reports backups, how mail leaves, and startup notes.
Is the backup key copied somewhere safe? Without it no backup can be read
(`goober-host backup key --confirm` once you've copied it).

## 5. Fix it by deploying again

```sh
just deploy kcmeatup.com --user customer
```

It upgrades everything and ends with `── checking everything this host needs`. The last line
must be `✓ every check passed`. Any `✗` line says what is wrong.

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
