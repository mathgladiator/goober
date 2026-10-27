#!/usr/bin/env python3
"""Plays the dumb terminal and an agent against a running goober host, end to end.

    tools/terminal_e2e.py --config demo/host.json [--url http://localhost:8080] [--email you@example.com]

It checks the contract of design/human/999.Mobile.md as goober keeps it
(design/machine/010.Terminal.md): RFC 8414 discovery, sign-in and consent in "the
browser", PKCE, the terminal's routes as a person, an agent signed in through the device
flow working the desk box, idempotent decisions, refresh rotation, and revocation.

The person signs in with an emailed code, read from the dev mailer's outbox, so it needs
the host's storage on this machine (local mode). It signs in as the tenant's first owner
unless --email says otherwise. What it creates in the desk is removed again, except the
approvals it decided, which stay as decided history. Standard library only.
"""
import argparse
import base64
import hashlib
import http.client
import json
import os
import re
import secrets
import sys
import time
import urllib.parse

UA = "dumb-terminal/0.0-e2e (test)"
APP_REDIRECT = "com.mathgladiator.dumb:/oauth/callback"
FAILURES = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILURES.append(what)
    return cond


class Client:
    def __init__(self, url):
        u = urllib.parse.urlsplit(url)
        self.host, self.port, self.origin = u.hostname, u.port or 80, f"{u.scheme}://{u.netloc}"

    def call(self, method, path, body=None, headers=None, form=None, js=None):
        h = dict(headers or {})
        if form is not None:
            body = urllib.parse.urlencode(form)
            h["Content-Type"] = "application/x-www-form-urlencoded"
        if js is not None:
            body = json.dumps(js)
            h["Content-Type"] = "application/json"
        c = http.client.HTTPConnection(self.host, self.port, timeout=10)
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read().decode("utf-8", "replace")
        out = (r.status, {k.lower(): v for k, v in r.getheaders()}, data)
        c.close()
        return out


def json_of(text):
    try:
        return json.loads(text)
    except ValueError:
        return {}


def newest_code(storage, after):
    box = os.path.join(storage, "mail", "outbox")
    for _ in range(50):
        files = sorted(f for f in os.listdir(box) if f.endswith(".eml")) if os.path.isdir(box) else []
        if files and files[-1] > after:
            m = re.search(r"is (\d{6})", open(os.path.join(box, files[-1]), encoding="utf-8").read())
            if m:
                return m.group(1)
        time.sleep(0.1)
    sys.exit("no sign-in code arrived in " + box)


def last_mail(storage):
    box = os.path.join(storage, "mail", "outbox")
    files = sorted(os.listdir(box)) if os.path.isdir(box) else []
    return files[-1] if files else ""


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--config", required=True)
    p.add_argument("--url", default="http://localhost:8080")
    p.add_argument("--email")
    a = p.parse_args()
    cfg = json.load(open(a.config))
    storage = os.path.join(os.path.dirname(os.path.abspath(a.config)), cfg.get("storage", "storage"))
    owners = [d.get("owner") for d in cfg["domains"].values() if d.get("owner")]
    email = a.email or (owners[0] if owners else sys.exit("no --email and no first owner in the config"))
    web = Client(a.url)
    same_site = {"Origin": web.origin}

    print("discovery")
    st, _, body = web.call("GET", "/.well-known/oauth-authorization-server")
    meta = json_of(body)
    check(st == 200 and meta.get("code_challenge_methods_supported") == ["S256"], "RFC 8414 metadata with S256")
    auth_path = urllib.parse.urlsplit(meta["authorization_endpoint"]).path
    token_path = urllib.parse.urlsplit(meta["token_endpoint"]).path

    print("the person signs in, in the browser")
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(12)
    q = urllib.parse.urlencode({"response_type": "code", "client_id": "dumb-terminal", "redirect_uri": APP_REDIRECT,
                                "code_challenge": challenge, "code_challenge_method": "S256", "state": state})
    st, h, _ = web.call("GET", f"{auth_path}?{q}")
    check(st == 303 and h.get("location", "").startswith("/identity/sign-in"), "signed out: sent to sign in")
    before = last_mail(storage)
    web.call("POST", "/identity/email/start", form={"email": email}, headers=same_site)
    st, h, _ = web.call("POST", "/identity/email/verify", form={"email": email, "code": newest_code(storage, before)},
                        headers=same_site)
    cookie = h.get("set-cookie", "").split(";")[0]
    check(st == 303 and cookie, "emailed code accepted, session cookie set")
    st, h, page = web.call("GET", f"{auth_path}?{q}", headers={"Cookie": cookie})
    check(st == 200 and "dumb terminal" in page and "com.mathgladiator.dumb:" in page, "consent names the app and its scheme")
    form = dict(urllib.parse.parse_qsl(q), decision="allow")
    st, h, _ = web.call("POST", auth_path, form=form, headers={"Cookie": cookie, **same_site})
    loc = h.get("location", "")
    back = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(loc).query))
    check(st == 303 and loc.startswith(APP_REDIRECT) and back.get("state") == state, "redirected to the app, state intact")

    print("the terminal trades the code")
    st, h, body = web.call("POST", token_path, form={"grant_type": "authorization_code", "client_id": "dumb-terminal",
                                                     "code": back.get("code", ""), "redirect_uri": APP_REDIRECT,
                                                     "code_verifier": verifier})
    tokens = json_of(body)
    check(st == 200 and tokens.get("token_type") == "Bearer" and "refresh_token" in tokens, "Bearer tokens with a refresh token")
    check(h.get("cache-control") == "no-store", "token response is no-store")

    def term(method, path, **kw):
        hdr = {"Authorization": "Bearer " + tokens["access_token"], "Accept": "application/json", "User-Agent": UA}
        hdr.update(kw.pop("headers", {}))
        return web.call(method, "/terminal/v1" + path, headers=hdr, **kw)

    st, _, body = term("GET", "/me")
    check(st == 200 and json_of(body).get("email") == email.lower(), "/me says who is signed in")
    st, _, body = term("GET", "/config")
    check(st == 200 and "name" in json_of(body), "/config has a name")
    st, _, _ = web.call("GET", "/terminal/v1/sync", headers={"Accept": "application/json"})
    check(st == 401, "no token: 401, not a redirect")

    print("an agent signs in with the device flow")
    st, _, body = web.call("POST", "/oauth/register", js={"client_name": "e2e agent", "redirect_uris": []})
    agent_id = json_of(body).get("client_id", "")
    check(st == 201 and agent_id.startswith("dyn_"), "dynamic registration gives an agent client")
    st, _, body = web.call("POST", "/oauth/device", form={"client_id": agent_id})
    dev = json_of(body)
    check(st == 200 and "user_code" in dev and "verification_uri_complete" not in dev, "device code, typed not linked")
    web.call("POST", "/oauth/device/verify", form={"step": "check", "user_code": dev.get("user_code", "")},
             headers={"Cookie": cookie, **same_site})
    st, _, _ = web.call("POST", "/oauth/device/verify",
                        form={"step": "decide", "decision": "allow", "user_code": dev.get("user_code", "")},
                        headers={"Cookie": cookie, **same_site})
    check(st == 200, "the person allows the agent")
    st, _, body = web.call("POST", token_path, form={"grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                                                     "client_id": agent_id, "device_code": dev.get("device_code", "")})
    agent = json_of(body)
    check(st == 200 and "access_token" in agent, "the agent collects its tokens")

    def desk(method, path, js=None):
        return web.call(method, "/desk/v1" + path, js=js,
                        headers={"Authorization": "Bearer " + agent.get("access_token", ""), "Accept": "application/json"})

    print("the agent works the desk")
    run = secrets.token_hex(3)
    st, _, _ = desk("PUT", "/settings", {"name": "goober", "default_location": {"lat": 41.88, "lon": -87.63, "label": "Chicago"}})
    check(st == 200, "settings: name and default location")
    today = time.strftime("%Y-%m-%d")
    alarm = {"kind": "alarm", "title": "Sunset walk", "body": "15 minutes until sunset.", "snooze_minutes": 5,
             "trigger": {"type": "solar", "event": "sunset", "offset_minutes": -15, "date": today}}
    st, _, _ = desk("PUT", f"/alarms/e2e-{run}", alarm)
    check(st == 200, "a solar alarm: 15 minutes before sunset today")
    st, _, _ = desk("PUT", f"/alarms/e2e-bad-{run}", {"kind": "alarm", "title": "x", "trigger": {"type": "solar", "event": "noonish"}})
    check(st == 400, "a bad trigger is refused")
    dash = {"title": "E2E", "pages": [{"id": "p", "title": "Page", "columns": 2, "widgets": [
        {"type": "metric", "label": "Runs", "value": "1"}, {"type": "markdown", "text": "**hello**"}]}]}
    st, _, _ = desk("PUT", f"/dashboards/e2e-{run}", dash)
    check(st == 200, "a dashboard")
    st, _, body = desk("POST", "/approvals", {"task": f"E2E approval {run}", "description": "**Ship it?**\n\n- one\n- two",
                                              "risk": "high"})
    approval = json_of(body).get("id", "")
    check(st == 201 and approval, "an approval request")
    hostile = "Pay to account \u202e9876\u202c today."
    st, _, _ = desk("POST", "/approvals", {"task": "Hostile", "description": hostile})
    check(st == 400, "a description with a bidi override is refused early")
    st, _, _ = desk("POST", "/approvals", {"task": "x", "extra": 1})
    check(st == 400, "objects are closed")

    print("agents can't decide")
    hdr = {"Authorization": "Bearer " + agent.get("access_token", ""), "Accept": "application/json"}
    st, _, _ = web.call("GET", "/terminal/v1/sync", headers=hdr)
    check(st == 403, "an agent token can't read the terminal's routes")
    key = secrets.token_hex(8)
    decision = {"decision": "approve", "decided_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "idempotency_key": key}
    st, _, _ = web.call("POST", f"/terminal/v1/approvals/{approval}/decision", js=decision,
                        headers={**hdr, "Idempotency-Key": key})
    check(st == 403, "an agent token can't post a decision")

    print("the terminal syncs and decides")
    st, h, body = term("GET", "/sync")
    snap = json_of(body)
    check(st == 200, "/sync answers")
    check(any(x.get("id") == approval for x in snap.get("approvals", [])), "the snapshot carries the pending approval")
    check(all(isinstance(x.get("id"), str) for x in snap.get("approvals", [])), "approval ids are strings")
    check(any(x.get("id") == f"e2e-{run}" and x["trigger"]["type"] == "solar" for x in snap.get("alarms", [])),
          "the snapshot carries the solar alarm")
    check(any(x.get("id") == f"e2e-{run}" for x in snap.get("dashboards", [])), "the dashboard menu lists it")
    check(snap.get("default_location", {}).get("label") == "Chicago", "default location")
    check(re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$", snap.get("server_time", "")) is not None, "server_time is RFC 3339 UTC")
    st, _, body = term("GET", f"/dashboards/e2e-{run}?range=7d")
    check(st == 200 and json_of(body).get("pages"), "one dashboard, with its pages")

    key = secrets.token_hex(8)
    decision = {"decision": "approve_with_feedback", "feedback": "Ship it Friday.",
                "decided_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "idempotency_key": key}
    st, _, _ = term("POST", f"/approvals/{approval}/decision", js=decision, headers={"Idempotency-Key": key})
    check(st == 204, "decision recorded")
    st, _, _ = term("POST", f"/approvals/{approval}/decision", js=decision, headers={"Idempotency-Key": key})
    check(st == 204, "the same key again is a success that changes nothing")
    other = dict(decision, idempotency_key=key + "x")
    st, _, _ = term("POST", f"/approvals/{approval}/decision", js=other, headers={"Idempotency-Key": key + "x"})
    check(st == 409, "a different key on a decided approval is 409")
    st, _, _ = term("POST", f"/approvals/{approval}/decision", js=dict(decision, feedback=None) | {"decision": "approve"},
                    headers={"Idempotency-Key": key})
    check(st == 400, "a null field is refused")
    st, _, body = term("GET", "/sync")
    check(not any(x.get("id") == approval for x in json_of(body).get("approvals", [])), "decided: gone from the snapshot")
    st, _, body = desk("GET", f"/approvals/{approval}")
    got = json_of(body)
    check(got.get("status") == "approved_with_feedback" and got.get("feedback") == "Ship it Friday.", "the agent reads the answer")

    print("devices and events")
    st, _, _ = term("POST", "/devices", js={"platform": "test", "app_version": "0.0", "device_name": f"e2e-{run}",
                                             "timezone": "America/Chicago"})
    check(st == 204, "device registered")
    ev = {"type": "alarm_snoozed", "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "idempotency_key": run,
          "alarm_id": f"e2e-{run}", "minutes": 5}
    check(term("POST", "/events", js=ev)[0] == 204, "event stored")
    check(term("POST", "/events", js=ev)[0] == 204, "a repeated event key is a success")
    check(term("POST", "/events", js=dict(ev, detail={"a": "b"}))[0] == 400, "the old 'detail' map is refused")
    st, _, body = desk("GET", "/devices")
    check(st == 200 and "push_token" not in body and "America/Chicago" in body, "agents see time zones, not push tokens")

    print("the agent works over MCP")
    def mcp(method, params=None):
        st, h, body = web.call("POST", "/mcp", js={"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}},
                               headers={"Authorization": "Bearer " + agent.get("access_token", "")})
        return st, json_of(body)
    st, h, _ = web.call("POST", "/mcp", js={"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
    check(st == 401 and "oauth-protected-resource" in h.get("www-authenticate", ""), "/mcp without a token points at OAuth")
    st, got = mcp("initialize", {"protocolVersion": "2025-06-18"})
    check(st == 200 and got.get("result", {}).get("protocolVersion") == "2025-06-18", "initialize")
    st, got = mcp("tools/list")
    names = [t["name"] for t in got.get("result", {}).get("tools", [])]
    check("desk_ask" in names and "desk_set_alarm" in names, "tools/list offers the desk's tools")
    check(not any(n.startswith("terminal") for n in names), "and nothing of the terminal's")
    ask = next((t for t in got.get("result", {}).get("tools", []) if t["name"] == "desk_ask"), {})
    check(ask.get("inputSchema", {}).get("additionalProperties") is False, "inputs are closed objects")
    st, got = mcp("tools/call", {"name": "desk_ask", "arguments": {"task": f"MCP approval {run}", "risk": "low"}})
    res = got.get("result", {})
    mcp_id = res.get("structuredContent", {}).get("id", "")
    check(not res.get("isError") and mcp_id, "desk_ask opens an approval")
    st, got = mcp("tools/call", {"name": "desk_ask", "arguments": {"task": "x", "risk": "extreme"}})
    check(got.get("result", {}).get("isError") is True, "a bad input is a tool error, and nothing runs")
    key = secrets.token_hex(8)
    st, _, _ = term("POST", f"/approvals/{mcp_id}/decision",
                    js={"decision": "deny", "feedback": "Not today.", "idempotency_key": key,
                        "decided_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())},
                    headers={"Idempotency-Key": key})
    check(st == 204, "the person denies it on the terminal")
    st, got = mcp("tools/call", {"name": "desk_check", "arguments": {"id": mcp_id}})
    sc = got.get("result", {}).get("structuredContent", {})
    check(sc.get("status") == "denied" and sc.get("feedback") == "Not today.", "desk_check reads the answer")

    print("refresh and sign-out")
    st, _, body = web.call("POST", token_path, form={"grant_type": "refresh_token", "client_id": "dumb-terminal",
                                                     "refresh_token": tokens["refresh_token"]})
    fresh = json_of(body)
    check(st == 200 and fresh.get("refresh_token") != tokens["refresh_token"], "refresh rotates the refresh token")
    old_access = tokens["access_token"]
    tokens = fresh
    check(web.call("GET", "/terminal/v1/me", headers={"Authorization": "Bearer " + old_access})[0] == 401,
          "the old access token ended with the rotation")
    check(term("GET", "/me")[0] == 200, "the new access token works")

    desk("DELETE", f"/alarms/e2e-{run}")
    desk("DELETE", f"/dashboards/e2e-{run}")
    for t, cid in ((tokens, "dumb-terminal"), (agent, agent_id)):
        web.call("POST", "/oauth/revoke", form={"client_id": cid, "token": t.get("refresh_token", "")})
    check(term("GET", "/me")[0] == 401, "after revocation the terminal is signed out")
    web.call("POST", "/identity/sign-out", headers={"Cookie": cookie, **same_site})

    print()
    if FAILURES:
        print(f"{len(FAILURES)} failed")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
