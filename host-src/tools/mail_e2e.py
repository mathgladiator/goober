#!/usr/bin/env python3
"""Mail in, the whole way (design/machine/024.MailIn.md): a real Stalwart, a real goober host,
and a box with mail routes, all on 127.0.0.1 high ports, no root.

  1. goober-host starts with mail.inbound; a box declares support@, refuse@, and a catch-all;
  2. Stalwart is installed (cached under build/stalwart), configured with the host's hook, and run;
  3. mail goes over SMTP: an unknown domain and a forged reply token are refused at RCPT, the
     box keeps what support@ gets (subject, sender, an attachment), refuse@'s 550 and later@'s
     451 come back as the SMTP answers, and an auto-reply is dropped before the box;
  4. nothing is queued in Stalwart.

    tools/mail_e2e.py [--host-bin build/dev/packages/host/goober-host] [--cache DIR] [--keep]
"""

import argparse
import base64
import email.message
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
SCRIPT = HERE / "stalwart.sh"
FAILURES = []

WAT = r'''(module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 8)
  (data (i32.const 0) "w")
  (data (i32.const 16) "INSERT INTO got (path, body) VALUES (:path, :body)")
  (data (i32.const 128) "application/json")
  (data (i32.const 160) "{\22error\22:\22No thanks\22}")
  (func $store
    (local $n i32)
    (local.set $n (call $read (i32.const 65536) (call $size)))
    (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 50) (i32.const 65536) (local.get $n))))
  (func (export "goober_route_support_inbox") (result i32) (call $store) (i32.const 0))
  (func (export "goober_route_inbox_all") (result i32) (call $store) (i32.const 0))
  (func (export "goober_route_ticket_reply") (result i32) (call $store) (i32.const 0))
  (func (export "goober_route_refuse_inbox") (result i32)
    (drop (call $respond (i32.const 400) (i32.const 128) (i32.const 16) (i32.const 160) (i32.const 21)))
    (i32.const 0))
  (func (export "goober_route_later_inbox") (result i32)
    (drop (call $respond (i32.const 503) (i32.const 128) (i32.const 16) (i32.const 160) (i32.const 0)))
    (i32.const 0))
)'''

MANIFEST = {
    "box": "desk", "domains": ["example.test"],
    "data": {"sqlite3": {"w": {"schema": "got1", "database": "got", "pattern": "sql",
                               "tables": {"got": {"read": True, "insert": True}}}}},
    "routes": {"mail": {
        "support@": {"receive": {"name": "support_inbox", "public": True, "attachments": 2}},
        "refuse@": {"receive": {"name": "refuse_inbox", "public": True}},
        "later@": {"receive": {"name": "later_inbox", "public": True}},
        "reply+{ticket : token}@": {"receive": {"name": "ticket_reply", "public": True}},
        "*@": {"receive": {"name": "inbox_all", "public": True}}}},
}
SCHEMA = {"schema": "got1", "tables": {"got": {"columns": {"path": {"type": "string"}, "body": {"type": "string"}}}}}


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


def mcp(port, token, name, args):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}/goober-operations-mcp", data=body, method="POST",
                                 headers={"content-type": "application/json", "authorization": "Bearer " + token,
                                          "host": "example.test"})
    with urllib.request.urlopen(req, timeout=30) as r:
        res = json.loads(r.read())["result"]
    if res.get("isError"):
        raise SystemExit(f"{name}: {res['content'][0]['text']}")
    return res.get("structuredContent")


def send(port, to, msg, mail_from="alice@sender.example.org"):
    """One message on its own connection; returns (rcpt code, data code, data text)."""
    time.sleep(0.25)  # Stalwart's default throttle: 5 connections a second from one address
    s = smtplib.SMTP("127.0.0.1", port, local_hostname="client.example.org", timeout=60)
    try:
        s.ehlo()
        s.mail(mail_from)
        code, text = s.rcpt(to)
        if code != 250:
            return code, 0, text.decode()
        dcode, dtext = s.data(msg.as_bytes())
        return code, dcode, dtext.decode()
    except smtplib.SMTPDataError as e:
        return 250, e.smtp_code, e.smtp_error.decode()
    finally:
        try:
            s.quit()
        except smtplib.SMTPException:
            s.close()


def message(subject, to="support@example.test", body="Do you have half a cow left?", **headers):
    m = email.message.EmailMessage()
    m["From"] = "Alice Lee <alice@sender.example.org>"
    m["To"] = to
    m["Subject"] = subject
    for k, v in headers.items():
        m[k.replace("_", "-")] = v
    m.set_content(body)
    return m


def wait_for(cond, what, seconds=60):
    for _ in range(seconds * 10):
        if cond():
            return
        time.sleep(0.1)
    raise SystemExit("timed out waiting for " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host-bin", default=str(HERE.parent / "build" / "dev" / "packages" / "host" / "goober-host"))
    p.add_argument("--cache", default=str(HERE.parent / "build" / "stalwart"))
    p.add_argument("--keep", action="store_true")
    a = p.parse_args()
    if not os.access(a.host_bin, os.X_OK):
        raise SystemExit(f"no goober-host at {a.host_bin}: build it first (just test)")

    print("── install")
    print("  " + sh("install", a.cache).strip().splitlines()[-1])
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="mail-e2e."))
    host = stalwart = None
    try:
        host_port, smtp_port, admin_port = free_port(), free_port(), free_port()
        hook_token = base64.urlsafe_b64encode(os.urandom(32)).decode()
        (tmp / "hook.token").write_text(hook_token + "\n")
        (tmp / "hook.token").chmod(0o600)
        op_token = "op-" + base64.urlsafe_b64encode(os.urandom(24)).decode()
        (tmp / "host").mkdir()
        (tmp / "host" / "host.json").write_text(json.dumps({
            "listen": {"address": "127.0.0.1", "port": host_port}, "storage": "storage",
            "platform-domain": "example.test", "domains": {"example.test": {"tenant": "kc"}},
            "operations": {"token": op_token}, "identity": {"mailer": "dev", "cookie-secure": False},
            "mail": {"inbound": {"token-file": str(tmp / "hook.token")}}}))

        print("── goober-host")
        host_log = tmp / "host.log"
        host = subprocess.Popen([a.host_bin, "--config", str(tmp / "host" / "host.json")],
                                stdout=open(host_log, "w"), stderr=subprocess.STDOUT)

        def health():
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{host_port}/goober/health", timeout=1):
                    return True
            except OSError:
                return False
        wait_for(health, "goober-host")
        mcp(host_port, op_token, "schema_put", {"schema": SCHEMA})
        mcp(host_port, op_token, "version_upload", {"manifest": MANIFEST, "wat": WAT})
        check(True, "the box with mail routes is up")

        print("── Stalwart")
        st = tmp / "st"
        (st / "bin").mkdir(parents=True)
        (st / "bin" / "stalwart").symlink_to(pathlib.Path(a.cache).resolve() / "bin" / "stalwart")
        sh("configure", str(st), "--smtp", f"127.0.0.1:{smtp_port}", "--admin", f"127.0.0.1:{admin_port}",
           "--hook-url", f"http://127.0.0.1:{host_port}/internal/routes/mta/stalwart",
           "--hook-token-file", str(tmp / "hook.token"), "--hostname", "mx.example.test",
           "--relay-domains", "example.test")
        st_log = tmp / "stalwart.log"
        stalwart = subprocess.Popen(["bash", str(SCRIPT), "run", str(st)], stdout=open(st_log, "w"),
                                    stderr=subprocess.STDOUT)

        def smtp_up():
            try:
                with socket.create_connection(("127.0.0.1", smtp_port), timeout=1) as c:
                    return c.recv(512).startswith(b"220")
            except OSError:
                return False
        wait_for(smtp_up, "Stalwart's SMTP", 90)
        check(True, "Stalwart answers SMTP")

        def rows():
            out = mcp(host_port, op_token, "db_query", {"tenant": "kc", "database": "got",
                                                         "sql": "SELECT path, body FROM got ORDER BY rowid"})
            return out.get("rows", [])

        print("── at the door (RCPT)")
        code, _, text = send(smtp_port, "nobody@elsewhere.example", message("hi"))
        check(code == 550, f"a domain goober doesn't serve is refused: {code} {text}")
        code, _, text = send(smtp_port, "nobody@deploy-check.invalid", message("hi"), mail_from="")
        check(code == 550, f"deploy's own check (a null sender, an address nobody serves) gets 550: {code} {text}")
        code, _, text = send(smtp_port, "reply+aaaaaaaaaaaaaaaaaaaaaaaa@example.test", message("hi"))
        check(code == 550, f"a forged reply token is refused: {code} {text}")

        print("── the box decides (DATA)")
        m = message("Half a cow?")
        m.add_attachment(b"%PDF-1.4\n", maintype="application", subtype="pdf", filename="order.pdf")
        code, dcode, text = send(smtp_port, "support@example.test", m)
        check(code == 250 and dcode == 250, f"support@ takes it: {dcode} {text}")
        got = rows()
        check(len(got) == 1 and got[0][0] == "mail:support@example.test", "the box has one message, for support@")
        if got:
            typed = json.loads(got[0][1])
            check(typed["subject"] == "Half a cow?", "its subject")
            check(typed["from"] == {"address": "alice@sender.example.org", "name": "Alice Lee"}, "its sender")
            check(typed["text"].strip() == "Do you have half a cow left?", "its text")
            check([x["filename"] for x in typed["attachments"]] == ["order.pdf"], "its attachment, as a handle")
        code, dcode, text = send(smtp_port, "refuse@example.test", message("hi", to="refuse@example.test"))
        check(dcode == 550 and "No thanks" in text, f"refuse@'s answer is the SMTP answer: {dcode} {text}")
        code, dcode, text = send(smtp_port, "later@example.test", message("hi", to="later@example.test"))
        check(dcode == 451, f"later@ defers: {dcode} {text}")
        code, dcode, text = send(smtp_port, "anyone@example.test",
                                 message("Out of office", to="anyone@example.test", Auto_Submitted="auto-replied"))
        check(dcode == 250, f"an auto-reply is taken: {dcode} {text}")
        check(len(rows()) == 1, "and dropped before any box")
        code, dcode, text = send(smtp_port, "anyone@example.test", message("Hello", to="anyone@example.test"))
        check(dcode == 250 and len(rows()) == 2 and rows()[1][0] == "mail:anyone@example.test",
              "the catch-all takes what nothing else claims")

        print("── afterwards")
        env = dict(line.split("=", 1) for line in (st / "etc" / "stalwart.env").read_text().splitlines()
                   if "=" in line and not line.startswith("#"))
        auth = "Basic " + base64.b64encode(env["STALWART_RECOVERY_ADMIN"].encode()).decode()
        body = json.dumps({"using": ["urn:ietf:params:jmap:core", "urn:stalwart:jmap"],
                           "methodCalls": [["x:QueuedMessage/query", {}, "0"]]}).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{admin_port}/jmap/", data=body, method="POST",
                                     headers={"content-type": "application/json", "authorization": auth})
        with urllib.request.urlopen(req, timeout=30) as r:
            queued = json.loads(r.read())["methodResponses"][0][1].get("ids")
        check(queued == [], "Stalwart's queue is empty")
        log = [json.loads(line) for line in (tmp / "host" / "storage" / "logs" / "access.log").read_text().splitlines()
               if "mail_in" in line]
        check(any(e.get("outcome") == "delivered" for e in log) and any(e.get("outcome") == "automatic_dropped" for e in log),
              f"the host's access log tells the story ({len(log)} hook calls)")
    finally:
        for proc in (stalwart, host):
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    proc.kill()
        if a.keep:
            print(f"── kept {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    if FAILURES:
        print(f"✗ {len(FAILURES)} failed")
        sys.exit(1)
    print("✓ all passed")


if __name__ == "__main__":
    main()
