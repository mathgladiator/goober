#!/usr/bin/env python3
"""Stalwart as goober's inbound SMTP front end, end to end, with the real Stalwart binary and
a stand-in hook (tools/fake_mta_hook.py), all on 127.0.0.1 high ports, no root:

  1. tools/stalwart.sh install (cached under build/stalwart), configure, and run;
  2. mail sent with smtplib: unknown recipients refused at RCPT, accepted mail handed to the
     hook at DATA (envelope, headers, body) and discarded, the hook's defer and reject passed
     on, the size limit, and a hook that answers "accept" refused by the queue quota;
  3. the bearer token on every hook request, nothing in Stalwart's queue, and a hook that is
     down answered with 451.

    tools/stalwart_e2e.py [--cache DIR] [--keep]
"""

import argparse
import base64
import json
import os
import pathlib
import shutil
import smtplib
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fake_mta_hook  # noqa: E402

SCRIPT = HERE / "stalwart.sh"
FAILURES = []
MAX_BYTES = 100000


def check(ok, what):
    print(("  ✓ " if ok else "  ✗ ") + what)
    if not ok:
        FAILURES.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def sh(*args):
    r = subprocess.run(["bash", str(SCRIPT), *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"stalwart.sh {args[0]} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def message(subject, body="Hello from the test.\r\n", extra=b""):
    head = (f"From: Alice <alice@sender.example.org>\r\nTo: Bob <bob@example.test>\r\n"
            f"Subject: {subject}\r\nMessage-ID: <{time.time_ns()}@sender.example.org>\r\n"
            f"Date: Wed, 28 Oct 2026 12:00:00 +0000\r\nX-Folded: one\r\n two\r\n"
            f"MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n").encode()
    return head + body.encode() + extra


def session(port):
    # Stalwart's default inbound throttle takes 5 connections a second from one address.
    time.sleep(0.25)
    s = smtplib.SMTP("127.0.0.1", port, local_hostname="client.example.org", timeout=30)
    s.ehlo()
    return s


def send(port, subject, rcpt="bob@example.test", **kw):
    """One message on its own connection; returns the (code, text) DATA got."""
    s = session(port)
    try:
        s.mail("alice@sender.example.org")
        code, text = s.rcpt(rcpt)
        if code != 250:
            return code, text
        s.putcmd("data")
        code, text = s.getreply()
        if code != 354:
            return code, text
        raw = smtplib.quotedata(message(subject, **kw).decode("latin-1")).encode("latin-1")
        s.send(raw + (b".\r\n" if raw.endswith(b"\r\n") else b"\r\n.\r\n"))
        return s.getreply()
    finally:
        try:
            s.quit()
        except smtplib.SMTPException:
            s.close()


def hook_requests(log):
    if not log.exists():
        return []
    return [json.loads(line) for line in log.read_text().splitlines() if line.strip()]


def admin_call(admin, env_file, *calls):
    values = dict(line.split("=", 1) for line in env_file.read_text().splitlines() if "=" in line and not line.startswith("#"))
    auth = "Basic " + base64.b64encode(values["STALWART_RECOVERY_ADMIN"].encode()).decode()
    body = json.dumps({"using": ["urn:ietf:params:jmap:core", "urn:stalwart:jmap"],
                       "methodCalls": [[m, a, str(i)] for i, (m, a) in enumerate(calls)]}).encode()
    req = urllib.request.Request(f"http://{admin}/jmap/", data=body, method="POST",
                                 headers={"content-type": "application/json", "authorization": auth})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())["methodResponses"]


def wait_for_smtp(port, proc, log):
    for _ in range(600):
        if proc.poll() is not None:
            raise SystemExit("Stalwart exited:\n" + log.read_text())
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1) as c:
                if c.recv(512).startswith(b"220"):
                    return
        except OSError:
            pass
        time.sleep(0.1)
    raise SystemExit("Stalwart didn't answer on SMTP:\n" + log.read_text())


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--cache", default=str(HERE.parent / "build" / "stalwart"),
                   help="where the Stalwart binary is installed once (default build/stalwart)")
    p.add_argument("--keep", action="store_true", help="keep the temporary directory")
    a = p.parse_args()

    print("── install")
    print("  " + sh("install", a.cache).strip().splitlines()[-1])
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stalwart-e2e."))
    proc = hook = None
    try:
        (tmp / "st" / "bin").mkdir(parents=True)
        (tmp / "st" / "bin" / "stalwart").symlink_to(pathlib.Path(a.cache).resolve() / "bin" / "stalwart")
        smtp_port, admin_port, hook_port = free_port(), free_port(), free_port()
        token = base64.urlsafe_b64encode(os.urandom(24)).decode()
        token_file = tmp / "hook.token"
        token_file.write_text(token + "\n")
        token_file.chmod(0o600)
        hook_log = tmp / "hook.jsonl"
        admin = f"127.0.0.1:{admin_port}"

        print("── configure")
        args = ["configure", str(tmp / "st"), "--smtp", f"127.0.0.1:{smtp_port}", "--admin", admin,
                "--hook-url", f"http://127.0.0.1:{hook_port}/internal/routes/mta/stalwart",
                "--hook-token-file", str(token_file), "--max-bytes", str(MAX_BYTES),
                "--hostname", "mx.example.test"]
        sh(*args)
        sh(*args)  # a rerun changes nothing and must still succeed
        check(True, "configure runs, and reruns")

        print("── run")
        hook = fake_mta_hook.serve(hook_port, token, str(hook_log))
        st_log = tmp / "stalwart.log"
        proc = subprocess.Popen(["bash", str(SCRIPT), "run", str(tmp / "st")],
                                stdout=open(st_log, "w"), stderr=subprocess.STDOUT)
        wait_for_smtp(smtp_port, proc, st_log)
        listening = subprocess.run(["ss", "-ltnpH"], capture_output=True, text=True).stdout
        mine = sorted(line.split()[3] for line in listening.splitlines() if f"pid={proc.pid}," in line
                      or any(f"pid={c}," in line for c in children(proc.pid)))
        check(mine == sorted([f"127.0.0.1:{smtp_port}", admin]) if mine else True,
              f"Stalwart listens only on SMTP and the admin API ({', '.join(mine) or 'ss shows no pids'})")

        print("── RCPT")
        s = session(smtp_port)
        ehlo = s.ehlo_resp.decode()
        check("AUTH" not in ehlo, "no AUTH offered")
        check(f"SIZE {MAX_BYTES}" in ehlo, f"SIZE {MAX_BYTES} advertised")
        s.mail("alice@sender.example.org")
        code, text = s.rcpt("eve@not-goober.example")
        check(code == 550 and text.startswith(b"5.1.1"), f"unknown recipient refused at RCPT: {code} {text.decode()}")
        code, text = s.rcpt("bob@example.test")
        check(code == 250, f"known recipient accepted: {code} {text.decode()}")
        s.quit()

        print("── DATA")
        before = len(hook_requests(hook_log))
        code, text = send(smtp_port, "Hello there", body="Line one\r\nLine two é\r\n")
        check(code == 250, f"discard is 250: {code} {text.decode()}")
        data = [r["request"] for r in hook_requests(hook_log)[before:] if r["request"].get("context", {}).get("stage") == "data"]
        check(len(data) == 1, "one data-stage hook request")
        if data:
            d = data[0]
            env = d.get("envelope") or {}
            check(env.get("from", {}).get("address") == "alice@sender.example.org"
                  and [t["address"] for t in env.get("to", [])] == ["bob@example.test"], "envelope: from and to")
            headers = d.get("message", {}).get("headers") or []
            names = [h[0] for h in headers]
            check(["From", "To", "Subject"] == [n for n in names if n in ("From", "To", "Subject")],
                  "headers: a list of [name, value] pairs, in order")
            check(dict((k, v) for k, v in headers).get("X-Folded") == " one\r\n two\r\n",
                  "header values are raw: the leading space, folding, and CRLF kept")
            contents = d.get("message", {}).get("contents", "")
            check(contents == "Line one\r\nLine two é\r\n", f"contents is the body only, CRLF: {contents!r}")
            check(d.get("message", {}).get("size", 0) > len(contents), "size counts the whole message")
            check("serverHeaders" not in d.get("message", {}), "no serverHeaders (Stalwart sends none)")

        print("── the hook's answers")
        code, text = send(smtp_port, "please defer this")
        check(code == 451 and text.startswith(b"4.3.0"), f"defer is 451 4.3.0: {code} {text.decode()}")
        code, text = send(smtp_port, "please reject this")
        check(code == 550 and text.startswith(b"5.7.1"), f"reject is 550 5.7.1: {code} {text.decode()}")
        code, text = send(smtp_port, "please accept this")
        check(code == 452 and text.startswith(b"4.3.1"), f"accept is refused by the queue quota, 452 4.3.1: {code} {text.decode()}")
        code, text = send(smtp_port, "Too big", body="x" * 76 + "\r\n", extra=(b"y" * 76 + b"\r\n") * (MAX_BYTES // 78 + 10))
        check(code == 552, f"over --max-bytes is 552: {code} {text.decode()}")
        # 8-bit, not UTF-8: Stalwart hands the hook a lossy UTF-8 string.
        before = len(hook_requests(hook_log))
        send(smtp_port, "latin-1", body="", extra=b"caf\xe9\r\n")
        latin = [r["request"] for r in hook_requests(hook_log)[before:] if r["request"].get("context", {}).get("stage") == "data"]
        check(bool(latin) and latin[0]["message"]["contents"] == "caf�\r\n",
              "non-UTF-8 bytes reach the hook as U+FFFD (Stalwart's from_utf8_lossy)")

        print("── afterwards")
        reqs = hook_requests(hook_log)
        check(bool(reqs) and all(r["authorized"] for r in reqs), f"every hook request carried the bearer token ({len(reqs)})")
        check(all(r["path"] == "/internal/routes/mta/stalwart" for r in reqs), "every hook request went to the configured path")
        res = admin_call(admin, tmp / "st" / "etc" / "stalwart.env", ("x:QueuedMessage/query", {}))
        name, out = res[0][0], res[0][1]
        check(name == "x:QueuedMessage/query" and out.get("ids") == [], f"Stalwart's queue is empty: {json.dumps(out)[:200]}")
        # Stalwart keeps its HTTP connection to the hook open, so "down" is a hook that
        # answers 503 rather than a closed port.
        hook.RequestHandlerClass.down = True
        s = session(smtp_port)
        s.mail("alice@sender.example.org")
        code, text = s.rcpt("bob@example.test")
        s.quit()
        check(code == 451 and text.startswith(b"4.3.5"), f"with the hook down, RCPT is 451 4.3.5: {code} {text.decode()}")
        log = st_log.read_text()
        check("build-error" not in log and "Configuration build error" not in log, "no configuration errors in Stalwart's log")
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
        if hook:
            hook.shutdown()
        if a.keep:
            print(f"── kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    if FAILURES:
        print(f"✗ {len(FAILURES)} failed")
        sys.exit(1)
    print("✓ all passed")


def children(pid):
    try:
        return [int(c) for c in pathlib.Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]
    except OSError:
        return []


if __name__ == "__main__":
    main()
