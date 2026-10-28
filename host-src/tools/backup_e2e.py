#!/usr/bin/env python3
"""Backups end to end, with the real binary and a stand-in S3 (tools/fake_s3.py):

  1. a fresh host with backups on: the AWS keys set from the command line, a page, a
     database row (through the leads box, if it is built), and an attachment;
  2. its first nightly, then an increment after more changes, through the operator tools;
  3. `goober-host rehydrate` into a devbox, which then runs as a sandbox and serves what the
     host had, at the latest moment and at the night's.

    tools/backup_e2e.py            (just backup-e2e)
"""

import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fake_s3  # noqa: E402

BIN = os.environ.get("GOOBER_HOST_BIN", str(HERE.parent / "build" / "dev" / "packages" / "host" / "goober-host"))
FAILURES = []
ACCESS, SECRET = "AKIDE2E", "e2e-secret"


def check(ok, what):
    print(("  ✓ " if ok else "  ✗ ") + what)
    if not ok:
        FAILURES.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run(*args, stdin=None, env=None):
    r = subprocess.run([BIN, *args], input=stdin, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        raise SystemExit(f"goober-host {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def start(config):
    p = subprocess.Popen([BIN, "--config", str(config), "--local"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    port = json.loads(pathlib.Path(config).read_text())["listen"]["port"]
    for _ in range(100):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/goober/health", timeout=1)
            return p
        except Exception:
            time.sleep(0.1)
    p.kill()
    raise SystemExit("the host didn't start: " + p.stderr.read())


def tool(port, token, name, args):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}/goober-operations-mcp", data=body, method="POST", headers={
        "content-type": "application/json", "accept": "application/json", "authorization": "Bearer " + token})
    with urllib.request.urlopen(req) as r:
        result = json.loads(r.read())["result"]
    text = result["content"][0]["text"]
    try:
        return (not result.get("isError")), json.loads(text)
    except ValueError:
        return (not result.get("isError")), text


def get(port, path, host="localhost"):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers={"host": host})
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="goober-backup-e2e-"))
    s3_port = free_port()
    server = fake_s3.serve(s3_port, str(work / "bucket"), ACCESS, SECRET)
    (work / "bucket").mkdir()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    host_dir = work / "host"
    host_dir.mkdir()
    port = free_port()
    config = host_dir / "host.json"
    config.write_text(json.dumps({
        "listen": {"port": port}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}},
        "backup": {"s3": {"bucket": "backups", "endpoint": f"http://127.0.0.1:{s3_port}", "prefix": "e2e"},
                   "databases-every-ms": 60000},
    }, indent=2))
    host = None
    try:
        print("── a host with backups on")
        run("key", "set", "aws_access_key_id", "--config", str(config), stdin=ACCESS)
        run("key", "set", "aws_secret_access_key", "--config", str(config), stdin=SECRET)
        key = run("backup", "key", "--config", str(config)).strip()
        check(len(key) == 64, "the backup key is printed once asked")
        token = run("token", "create", "--config", str(config), "--name", "e2e").strip()
        host = start(config)
        ok, _ = tool(port, token, "content_put", {"tenant": "playground", "path": "index.md", "text": "# Before"})
        check(ok, "a page")
        png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="
        ok, att = tool(port, token, "attachment_put", {"tenant": "playground", "base64": png, "name": "dot.png"})
        check(ok, "an attachment")
        ok, r = tool(port, token, "key_set", {"scope": "system", "name": "aws_secret_access_key", "value": "x"})
        check(not ok and "host's own key" in str(r), "agents can't change the host's AWS keys")

        print("── the nightly, the asset, and an increment")
        ok, r = tool(port, token, "backup_run", {"what": "nightly"})
        check(ok, f"a nightly runs: {r}")
        ok, r = tool(port, token, "backup_run", {"what": "assets"})
        check(ok and r.get("uploaded", 0) >= 0, "the asset queue runs")
        time.sleep(1.1)  # increments are named to the second
        ok, _ = tool(port, token, "content_put", {"tenant": "playground", "path": "index.md", "text": "# After"})
        ok, r = tool(port, token, "backup_run", {"what": "increment"})
        check(ok, f"an increment runs: {r}")
        ok, status = tool(port, token, "backup_status", {"points": True})
        points = status.get("restore_points", [])
        check(ok and len(points) == 2, f"two restore points: {[p['at'] for p in points]}")
        check(status.get("assets", {}).get("uploaded") == 1, "the attachment is in the bucket")
        check("key_warning" in status, "the unconfirmed key is called out")
        run("backup", "key", "--confirm", "--config", str(config))
        ok, status = tool(port, token, "backup_status", {})
        check("key_warning" not in status, "and the warning goes once it's confirmed")
        listed = run("backup", "list", "--config", str(config))
        check("2 restore points" in listed, "the CLI lists them")
        objects = [p for p in (work / "bucket" / "backups").rglob("*") if p.is_file()]
        check(all(p.read_bytes().startswith(b"GOOBERS1") for p in objects), f"all {len(objects)} objects are sealed")
    finally:
        if host:
            host.terminate()
            host.wait()

    print("── rehydrate a devbox")
    env = dict(os.environ, AWS_ACCESS_KEY_ID=ACCESS, AWS_SECRET_ACCESS_KEY=SECRET, GOOBER_BACKUP_KEY=key)
    dev_port = free_port()
    out = run("rehydrate", str(work / "devbox"), "--from", str(config), "--port", str(dev_port), env=env)
    check("It is a sandbox" in out, "rehydrated: " + out.splitlines()[0])
    dev_config = json.loads((work / "devbox" / "host.json").read_text())
    check(dev_config.get("sandbox") is True and "backup" not in dev_config, "the devbox is a sandbox without backups")
    check(not (work / "devbox" / "storage" / "keys").exists(), "the production keys stayed behind")
    devbox = start(work / "devbox" / "host.json")
    try:
        st, page = get(dev_port, "/")
        check(st == 200 and b"After" in page, "the devbox serves the latest page")
        st, img = get(dev_port, att["url"]) if isinstance(att, dict) else (0, b"")
        check(st == 200 and img.startswith(b"\x89PNG"), "and the attachment")
    finally:
        devbox.terminate()
        devbox.wait()
    night = points[0]["at"] if points else ""
    out = run("rehydrate", str(work / "night"), "--from", str(config), "--at", night[:19].replace("Z", ""), "--port",
              str(dev_port), env=env)
    page = (work / "night" / "storage" / "content" / "playground" / "index.md")
    check(page.exists() and "Before" in page.read_text(), "the night's moment has the page as it was")
    bad = subprocess.run([BIN, "rehydrate", str(work / "bad"), "--from", str(config)], capture_output=True, text=True,
                         env=dict(env, GOOBER_BACKUP_KEY="00" * 32))
    check(bad.returncode != 0 and "wrong backup key" in bad.stderr, "the wrong key reads nothing")
    server.shutdown()
    if FAILURES:
        print(f"\n{len(FAILURES)} failed; files kept in {work}")
        sys.exit(1)
    shutil.rmtree(work)
    print("\nall backup checks passed")


if __name__ == "__main__":
    main()
