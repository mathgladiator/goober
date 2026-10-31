#!/usr/bin/env python3
"""Tests tenant_edit.py (the deploy's --add-tenant edits) on temporary files, and checks each
edited host.json with goober-host itself.

  tenant_edit_test.py [PATH_TO_GOOBER_HOST]
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "tenant_edit.py")
BIN = sys.argv[1] if len(sys.argv) > 1 else ""

CADDY = """# written by setup
{
  email ops@shop.example
}
shop.example, ops.shop.example {
  encode zstd gzip
  respond /internal/* 404
  reverse_proxy 127.0.0.1:8080 {
    health_uri /goober/health
  }
  handle_errors {
    @down expression `{err.status_code} in [502, 503, 504]`
    handle @down {
      respond `{"error":"temporarily_unavailable"}` 503
    }
  }
}
"""

HOST = {
    "listen": {"port": 8080},
    "storage": "./storage",
    "platform-domain": "shop.example",
    "domains": {"shop.example": {"tenant": "shop"}, "www.shop.example": {"redirect-to": "shop.example"}},
    "operations": {"domain": "ops.shop.example"},
    "tenants": {"shop": {"postal-address": "1 Main St, Springfield"}},
}

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("✗", what)


def run(*args):
    # Fed on stdin, as the deploy runs it.
    with open(SCRIPT) as f:
        return subprocess.run([sys.executable, "-I", "-", *args], stdin=f, capture_output=True, text=True)


with tempfile.TemporaryDirectory() as tmp:
    host = os.path.join(tmp, "host.json")
    caddy = os.path.join(tmp, "Caddyfile")

    def reset():
        with open(host, "w") as f:
            json.dump(HOST, f)
        os.chmod(host, 0o640)
        with open(caddy, "w") as f:
            f.write(CADDY)

    reset()
    # inspect: free names, taken names, and the suggestion.
    r = json.loads(run("inspect", host, "Bobs-Plumbing.example").stdout)
    check(r["ok"] and r["suggested"] == "bobs_plumbing" and r["tenants"] == ["shop"] and r["www"], "inspect a free name: " + str(r))
    for taken, says in (("shop.example", "already served"), ("www.shop.example", "redirects"),
                        ("ops.shop.example", "panel"), ("10.0.0.1", "address"), ("app.localhost", "local"),
                        ("not a name", "isn't a domain")):
        r = json.loads(run("inspect", host, taken).stdout)
        check(not r["ok"] and says in r["problem"], "inspect " + taken + ": " + str(r))
    check(json.loads(run("inspect", host, "9lives.example").stdout)["suggested"] == "t_9lives", "a tenant can't start with a digit")

    # host-json: a new tenant with everything, and www.
    r = run("host-json", host, "--domain", "bobs.example", "--tenant", "bobs", "--owner", "Bob@Bobs.example",
            "--postal", "2 Pipe Rd, Springfield", "--terms", "/terms", "--privacy", "/privacy", "--www")
    check(r.returncode == 0, "host-json for a new tenant: " + r.stderr)
    cfg = json.load(open(host))
    check(cfg["domains"]["bobs.example"] == {"tenant": "bobs", "owner": "bob@bobs.example"}, "the domain entry")
    check(cfg["domains"]["www.bobs.example"] == {"redirect-to": "bobs.example"}, "the www. redirect")
    check(cfg["tenants"]["bobs"] == {"postal-address": "2 Pipe Rd, Springfield", "terms": "/terms", "privacy": "/privacy"},
          "the tenant's settings")
    check(cfg["tenants"]["shop"] == HOST["tenants"]["shop"], "other tenants untouched")
    check(oct(os.stat(host).st_mode & 0o777) == oct(0o640), "the file keeps its mode")

    # Joining an existing tenant keeps what it has.
    r = run("host-json", host, "--domain", "shop2.example", "--tenant", "shop", "--postal", "9 Other St")
    check(r.returncode == 0, "join an existing tenant: " + r.stderr)
    check(json.load(open(host))["tenants"]["shop"]["postal-address"] == "1 Main St, Springfield", "an existing postal address stays")
    # Refusals change nothing.
    before = open(host).read()
    for args, says in ((["--domain", "bobs.example", "--tenant", "x"], "already served"),
                       (["--domain", "new.example", "--tenant", "Bad-Name"], "tenant name"),
                       (["--domain", "new.example", "--tenant", "n", "--owner", "nope"], "email"),
                       (["--domain", "new.example", "--tenant", "n", "--postal", "a\nb"], "one line"),
                       (["--domain", "new.example", "--tenant", "n", "--terms", "terms"], "path"),
                       (["--domain", "shop.example", "--tenant", "n"], "already served")):
        r = run("host-json", host, *args)
        check(r.returncode != 0 and says in r.stderr, "refused " + " ".join(args) + ": " + r.stderr)
    check(open(host).read() == before, "a refusal changes nothing")

    # The host accepts what was written.
    if BIN:
        r = subprocess.run([BIN, "check", "--config", host], capture_output=True, text=True)
        check(r.returncode == 0, "goober-host check accepts the edited host.json: " + r.stdout + r.stderr)

    # caddy: a new block, a copy of goober's, after the others.
    r = run("caddy", caddy, "--domain", "bobs.example", "--www")
    check(r.returncode == 0, "caddy: " + r.stderr)
    text = open(caddy).read()
    check(text.startswith(CADDY.rstrip("\n")), "the old blocks are untouched")
    new = text[len(CADDY.rstrip("\n")):]
    check(new.startswith("\n\nbobs.example, www.bobs.example {\n  encode zstd gzip"), "the new block's site line: " + repr(new[:60]))
    check(new.count("health_uri /goober/health") == 1 and new.count("handle_errors") == 1 and new.endswith("}\n"),
          "the new block copies goober's body")
    check(text.count("{") == text.count("}"), "braces balance")
    r = run("caddy", caddy, "--domain", "bobs.example")
    check(r.returncode != 0 and "already serves" in r.stderr, "caddy refuses a name it serves")
    r = run("caddy", caddy, "--domain", "x.example", "--www")
    check(r.returncode == 0, "another block")
    with open(caddy, "w") as f:
        f.write("example.org {\n  respond 200\n}\n")
    r = run("caddy", caddy, "--domain", "bobs.example")
    check(r.returncode != 0 and "no site block proxies to goober" in r.stderr, "caddy needs goober's block to copy")

    # caddy validate, when Caddy is here.
    reset()
    run("caddy", caddy, "--domain", "bobs.example", "--www")
    try:
        r = subprocess.run(["caddy", "validate", "--config", caddy, "--adapter", "caddyfile"], capture_output=True, text=True)
        check(r.returncode == 0, "caddy validate: " + r.stderr[-400:])
    except FileNotFoundError:
        print("  (no caddy here: not validated)")

    # dns: shape only (no network needed).
    r = run("dns", "localhost", "localhost")
    out = json.loads(r.stdout)
    check("localhost" in out and out["localhost"]["dns"] in ("ready", "missing", "unknown", "elsewhere"), "dns output: " + r.stdout)

if failures:
    print(len(failures), "failed")
    sys.exit(1)
print("✓ tenant_edit")
