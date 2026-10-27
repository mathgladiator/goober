#!/usr/bin/env python3
"""A tiny client for a goober host's operator MCP endpoint (stdlib only).

    tools/goober.py tools                         list the operator tools
    tools/goober.py call <tool> [key=value ...]   call a tool (values parse as JSON when they can)
    tools/goober.py deploy <manifest.json> <module.wat|module.wasm>
    tools/goober.py push <tenant> <directory> [KEY=VALUE ...]
                                                  upload a directory as content, replacing
                                                  each KEY in text files with VALUE
    tools/goober.py schema <file.schema.json>     store a database schema
    tools/goober.py attach <tenant> <url|file> [--name N] [--alt TEXT]
                                                  store an image and print its URL; an
                                                  attachment with the same name is reused

The host comes from a config file: GOOBER_CONFIG, or else demo/host.json at the
repo root. GOOBER_URL overrides the endpoint (default: the config's listen port on
127.0.0.1) and GOOBER_TOKEN the bearer token (default: the config's operations.token).
"""

import base64
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent


def config():
    path = pathlib.Path(os.environ.get("GOOBER_CONFIG", REPO / "demo" / "host.json"))
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        sys.exit(f"no host config at {path}; run `just setup`, or set GOOBER_CONFIG")


def url():
    if "GOOBER_URL" in os.environ:
        base = os.environ["GOOBER_URL"]
    else:
        base = f"http://127.0.0.1:{config().get('listen', {}).get('port', 8080)}"
    return base.rstrip("/") + "/goober-operations-mcp"


def token():
    if "GOOBER_TOKEN" in os.environ:
        return os.environ["GOOBER_TOKEN"]
    t = config().get("operations", {}).get("token")
    if not t:
        sys.exit("the config has no operations.token; set GOOBER_TOKEN to a token from `just mcp`")
    return t


def rpc(method, params=None):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(url(), data=body, method="POST", headers={
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
        # The types document the manifest names sits beside it (036).
        if manifest.get("types"):
            args["types"] = json.loads((pathlib.Path(argv[2]).parent / manifest["types"]).read_text())
        if module.suffix == ".wat":
            args["wat"] = module.read_text()
        else:
            args["wasm_base64"] = base64.b64encode(module.read_bytes()).decode()
        print(call("version_upload", args))
    elif cmd == "push":
        tenant, root = argv[2], pathlib.Path(argv[3])
        replace = dict(a.split("=", 1) for a in argv[4:])
        for f in sorted(p for p in root.rglob("*") if p.is_file()):
            rel = f.relative_to(root).as_posix()
            data = f.read_bytes()
            try:
                text = data.decode("utf-8")
                for k, v in replace.items():
                    text = text.replace(k, v)
                args = {"tenant": tenant, "path": rel, "text": text}
            except UnicodeDecodeError:
                args = {"tenant": tenant, "path": rel, "base64": base64.b64encode(data).decode()}
            call("content_put", args)
            print(f"put {tenant}/{rel} ({len(data)} bytes)")
    elif cmd == "schema":
        print(call("schema_put", {"schema": json.loads(pathlib.Path(argv[2]).read_text())}))
    elif cmd == "attach":
        tenant, source, rest = argv[2], argv[3], argv[4:]
        opts = {rest[i].lstrip("-"): rest[i + 1] for i in range(0, len(rest) - 1, 2)}
        name = opts.get("name") or source.rstrip("/").split("/")[-1]
        for a in json.loads(call("attachment_list", {"tenant": tenant})):
            if a["name"] == name:
                print(a["url"])
                return
        args = {"tenant": tenant, "name": name, "alt": opts.get("alt", "")}
        if source.startswith("https://"):
            args["url"] = source
        else:
            args["base64"] = base64.b64encode(pathlib.Path(source).read_bytes()).decode()
        print(json.loads(call("attachment_put", args))["url"])
    else:
        sys.exit(f"unknown command {cmd}; try --help")


if __name__ == "__main__":
    main(sys.argv)
