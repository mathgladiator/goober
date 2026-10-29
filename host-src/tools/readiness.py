#!/usr/bin/env python3
"""Checks a deployed goober host from the outside, the way people and agents reach it.

  readiness.py https://kcmeatup.com \
      --expect /places/rye-leawood text/html \
      --expect /sitemap.xml application/xml \
      --expect /calculate/meat-macros.csv text/csv \
      --box kc_locations --box kc_access

Always checked: /goober/ready (build and every subsystem), /, OAuth and identity discovery,
and that /mcp asks for OAuth the way MCP clients expect. With GOOBER_TOKEN (an operator
token), also: /goober-operations-mcp initialize and tools/list, and that each --box is
loaded (--box name@version to require a version). With GOOBER_SITE_TOKEN (a bearer from the
site's realm), /mcp initialize and tools/list too. Exits 1 if anything fails.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request


def request(url, method="GET", body=None, headers=None, timeout=20):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    if data is not None:
        req.add_header("content-type", "application/json")
    req.add_header("user-agent", "goober-readiness/1")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict((k.lower(), v) for k, v in r.headers.items()), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict((k.lower(), v) for k, v in e.headers.items()), e.read()
    except Exception as e:  # noqa: BLE001 (any failure to connect is a failed check)
        return 0, {}, str(e).encode()


def mcp(url, token):
    """initialize, then tools/list: the tool names, or raises with what went wrong."""
    auth = {"authorization": "Bearer " + token}
    init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "readiness", "version": "1"}}}
    status, _, body = request(url, "POST", init, auth)
    if status != 200 or b'"result"' not in body:
        raise RuntimeError(f"initialize: HTTP {status} {body[:200]!r}")
    status, _, body = request(url, "POST", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, auth)
    if status != 200:
        raise RuntimeError(f"tools/list: HTTP {status} {body[:200]!r}")
    return [t["name"] for t in json.loads(body)["result"]["tools"]]


def call(url, token, tool, arguments=None):
    body = {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": tool, "arguments": arguments or {}}}
    status, _, raw = request(url, "POST", body, {"authorization": "Bearer " + token})
    result = json.loads(raw)["result"]
    if status != 200 or result.get("isError"):
        raise RuntimeError(f"{tool}: {result['content'][0]['text'][:200]}")
    return json.loads(result["content"][0]["text"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base", help="like https://kcmeatup.com")
    ap.add_argument("--expect", nargs=2, action="append", default=[], metavar=("PATH", "TYPE"),
                    help="a path that must answer 200 with a content type starting with TYPE")
    ap.add_argument("--box", action="append", default=[], help="a box that must be loaded: name or name@version")
    ap.add_argument("--build", help="the build (commit prefix) /goober/ready must report")
    args = ap.parse_args()
    base = args.base.rstrip("/")
    failed = []

    def check(what, ok, detail=""):
        print(("  ✓ " if ok else "  ✗ ") + what + (f"  ({detail})" if detail else ""))
        if not ok:
            failed.append(what)

    status, _, body = request(base + "/goober/ready")
    ready = {}
    try:
        ready = json.loads(body)
    except ValueError:
        pass
    bad = [k for k, v in ready.get("checks", {}).items() if not v.get("ok")]
    check("/goober/ready", status == 200 and ready.get("ok") is True,
          f"build {ready.get('build')}, up {ready.get('uptime_ms', 0) // 1000}s" if status == 200 else
          f"HTTP {status}" + (f", failing: {', '.join(bad)}" if bad else "") + (f" {body[:120]!r}" if not ready else ""))
    if args.build:
        check(f"build is {args.build}", str(ready.get("build", "")).startswith(args.build), str(ready.get("build")))

    for path, kind in [("/", "text/html")] + [tuple(e) for e in args.expect]:
        status, headers, _ = request(base + path)
        ctype = headers.get("content-type", "")
        check(f"GET {path} is 200 {kind}", status == 200 and ctype.startswith(kind), f"HTTP {status} {ctype}")

    for path in ["/.well-known/oauth-authorization-server", "/.well-known/oauth-protected-resource/mcp",
                 "/.well-known/oauth-authorization-server/oauth/operator",
                 "/.well-known/oauth-protected-resource/goober-operations-mcp"]:
        status, headers, _ = request(base + path)
        check(f"GET {path}", status == 200 and headers.get("content-type", "").startswith("application/json"), f"HTTP {status}")
    status, headers, _ = request(base + "/identity/sign-in")
    check("GET /identity/sign-in", status == 200, f"HTTP {status}")

    site_token = os.environ.get("GOOBER_SITE_TOKEN", "")
    if site_token:
        try:
            check("/mcp initialize and tools/list", True, f"{len(mcp(base + '/mcp', site_token))} tools")
        except Exception as e:  # noqa: BLE001
            check("/mcp initialize and tools/list", False, str(e))
    else:
        status, headers, _ = request(base + "/mcp", "POST", {"jsonrpc": "2.0", "id": 1, "method": "initialize"})
        check("/mcp asks MCP clients to sign in", status == 401 and "resource_metadata" in headers.get("www-authenticate", ""),
              f"HTTP {status}")

    token = os.environ.get("GOOBER_TOKEN", "")
    ops = base + "/goober-operations-mcp"
    if token:
        try:
            tools = mcp(ops, token)
            check("/goober-operations-mcp initialize and tools/list", True, f"{len(tools)} tools")
            health = call(ops, token, "host_health")
            loaded = {b["box"]: b["version"] for b in health.get("boxes_loaded", [])}
            for want in args.box:
                name, _, version = want.partition("@")
                have = loaded.get(name)
                check(f"box {want} is loaded", have is not None and (not version or have.startswith(version)),
                      have or "not loaded")
            if health.get("startup_notes"):
                print("    startup notes: " + "; ".join(health["startup_notes"]))
        except Exception as e:  # noqa: BLE001
            check("/goober-operations-mcp initialize and tools/list", False, str(e))
    else:
        status, headers, _ = request(ops, "POST", {"jsonrpc": "2.0", "id": 1, "method": "initialize"})
        check("/goober-operations-mcp asks for a token", status == 401, f"HTTP {status}")
        if args.box:
            print("    (set GOOBER_TOKEN to check boxes: " + ", ".join(args.box) + ")")

    print(("✓ ready" if not failed else f"✗ {len(failed)} check(s) failed: " + "; ".join(failed)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
