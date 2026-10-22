#!/usr/bin/env python3
"""A tiny client for a goober host's operator MCP endpoint (stdlib only).

    tools/goober.py tools                         list the operator tools
    tools/goober.py call <tool> [key=value ...]   call a tool (values parse as JSON when they can)
    tools/goober.py deploy <manifest.json> <module.wat|module.wasm>
    tools/goober.py push <tenant> <directory>     upload a directory as static content

The endpoint and token come from GOOBER_URL (default http://127.0.0.1:8080) and
GOOBER_TOKEN (default: operations.token from dev/host.json).
"""

import base64
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
URL = os.environ.get("GOOBER_URL", "http://127.0.0.1:8080").rstrip("/") + "/goober-operations-mcp"


def token():
    if "GOOBER_TOKEN" in os.environ:
        return os.environ["GOOBER_TOKEN"]
    cfg = json.loads((HERE.parent / "dev" / "host.json").read_text())
    return cfg["operations"]["token"]


def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers={
        "content-type": "application/json",
        "accept": "application/json, text/event-stream",
        "authorization": "Bearer " + token(),
    })
    try:
        with urllib.request.urlopen(req) as r:
            reply = json.loads(r.read())
    except urllib.error.HTTPError as e:
        sys.exit(f"{e.code}: {e.read().decode()}")
    if "error" in reply:
        sys.exit(f"error: {reply['error']['message']}")
    return reply["result"]


def call(tool, args):
    result = rpc("tools/call", {"name": tool, "arguments": args})
    text = result["content"][0]["text"]
    if result.get("isError"):
        sys.exit(f"{tool} failed: {text}")
    return text


def parse_value(v):
    try:
        return json.loads(v)
    except ValueError:
        return v


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return
    cmd = argv[1]
    if cmd == "tools":
        for t in rpc("tools/list")["tools"]:
            print(f"{t['name']:16} {t['description']}")
    elif cmd == "call":
        args = dict(a.split("=", 1) for a in argv[3:])
        print(call(argv[2], {k: parse_value(v) for k, v in args.items()}))
    elif cmd == "deploy":
        manifest = json.loads(pathlib.Path(argv[2]).read_text())
        module = pathlib.Path(argv[3])
        args = {"manifest": manifest}
        if module.suffix == ".wat":
            args["wat"] = module.read_text()
        else:
            args["wasm_base64"] = base64.b64encode(module.read_bytes()).decode()
        print(call("version_upload", args))
    elif cmd == "push":
        tenant, root = argv[2], pathlib.Path(argv[3])
        for f in sorted(p for p in root.rglob("*") if p.is_file()):
            rel = f.relative_to(root).as_posix()
            data = f.read_bytes()
            try:
                args = {"tenant": tenant, "path": rel, "text": data.decode("utf-8")}
            except UnicodeDecodeError:
                args = {"tenant": tenant, "path": rel, "base64": base64.b64encode(data).decode()}
            call("content_put", args)
            print(f"put {tenant}/{rel} ({len(data)} bytes)")
    else:
        sys.exit(f"unknown command {cmd}; try --help")


if __name__ == "__main__":
    main(sys.argv)
