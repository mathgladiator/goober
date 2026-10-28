#!/usr/bin/env python3
"""A stand-in for goober's MTA hook (/internal/routes/mta/stalwart), for trying Stalwart
without a host (stdlib only).

    tools/fake_mta_hook.py --port 18192 --token-file PATH --log FILE

With `down` set on the handler class (the e2e does), every request gets 503, and Stalwart
answers 451 4.3.5. Every request must carry `Authorization: Bearer <the token in PATH>`; others get 401, which
Stalwart turns into 451 4.3.5 (tempFailOnError). Each request is appended to FILE as one JSON
line: {"path", "authorized", "request"}, where request is the JSON Stalwart sent. Answers:

  rcpt: the newest recipient ends in @example.test -> accept; else reject 550 5.1.1
  data: Subject has "defer"  -> reject 451 4.3.0
        Subject has "reject" -> reject 550 5.7.1
        Subject has "accept" -> accept (Stalwart would queue it; the queue quota refuses)
        otherwise            -> discard (Stalwart answers 250 and keeps nothing)
  other stages: accept
"""

import argparse
import http.server
import json
import threading


def decide(req):
    stage = req.get("context", {}).get("stage")
    if stage == "rcpt":
        rcpts = (req.get("envelope") or {}).get("to") or []
        newest = rcpts[-1]["address"] if rcpts else ""
        if newest.endswith("@example.test"):
            return {"action": "accept"}
        return {"action": "reject", "response": {"status": 550, "enhanced_status": "5.1.1",
                                                 "message": "No such recipient here"}}
    if stage == "data":
        headers = (req.get("message") or {}).get("headers") or []
        subject = next((v for k, v in headers if k.lower() == "subject"), "").lower()
        if "defer" in subject:
            return {"action": "reject", "response": {"status": 451, "enhanced_status": "4.3.0",
                                                     "message": "Try again later"}}
        if "reject" in subject:
            return {"action": "reject", "response": {"status": 550, "enhanced_status": "5.7.1",
                                                     "message": "Refused by goober"}}
        if "accept" in subject:
            return {"action": "accept"}
        return {"action": "discard"}
    return {"action": "accept"}


class Handler(http.server.BaseHTTPRequestHandler):
    token = None
    log_path = None
    down = False  # set on the server's RequestHandlerClass: answer 503, as a goober that is down
    lock = threading.Lock()
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length") or 0))
        authorized = self.headers.get("authorization") == "Bearer " + self.token
        try:
            req = json.loads(raw)
        except ValueError:
            req = {"unparsed": raw.decode(errors="replace")}
        with self.lock, open(self.log_path, "a") as f:
            f.write(json.dumps({"path": self.path, "authorized": authorized, "request": req}) + "\n")
        if self.down:
            self.close_connection = True
            return self.reply(503, {"error": "down"})
        if not authorized:
            return self.reply(401, {"error": "unauthorized"})
        self.reply(200, decide(req))


def serve(port, token, log_path, host="127.0.0.1"):
    """Starts the hook in a thread; returns the server (call .shutdown() to stop it)."""
    handler = type("BoundHandler", (Handler,), {"token": token, "log_path": log_path})
    server = http.server.ThreadingHTTPServer((host, port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", type=int, default=18192)
    p.add_argument("--token-file", required=True)
    p.add_argument("--log", required=True)
    a = p.parse_args()
    with open(a.token_file) as f:
        token = f.read().strip()
    handler = type("BoundHandler", (Handler,), {"token": token, "log_path": a.log})
    print(f"fake MTA hook on http://127.0.0.1:{a.port}/, logging to {a.log}", flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", a.port), handler).serve_forever()


if __name__ == "__main__":
    main()
