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
    tools/goober.py source list                   the host's source projects (521)
    tools/goober.py source pull <project> <dir> [rev]
                                                  write a project's files into dir
    tools/goober.py source push <dir> <message>   commit dir's changes to its project
    tools/goober.py source new <project> <dir> [<template>]
                                                  create a project and pull it into dir
    tools/goober.py source build|deploy <project> [rev]

The host comes from a config file: GOOBER_CONFIG, or else demo/host.json at the
repo root. GOOBER_URL overrides the endpoint (default: the config's listen port on
127.0.0.1) and GOOBER_TOKEN the bearer token (default: the config's operations.token).
"""

import base64
import json
import os
import pathlib
import sys
import time
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
    for attempt in range(6):
        try:
            with urllib.request.urlopen(req) as r:
                reply = json.loads(r.read())
            break
        except urllib.error.HTTPError as e:
            # Rate limited: wait as told and try again, so a big push finishes.
            if e.code == 429 and attempt < 5:
                time.sleep(min(60, int(e.headers.get("retry-after", "2") or 2)))
                continue
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


# Source projects (design/machine/020.Source.md): a directory pulled from a project remembers
# which project and revision it came from in .goober-source.json, so push can commit on top.
SKIP_DIRS = {".git", "target", ".goober-cache"}
MARK = ".goober-source.json"


def local_files(root):
    out = {}
    for f in sorted(root.rglob("*")):
        rel = f.relative_to(root)
        if not f.is_file() or rel.as_posix() == MARK or any(part in SKIP_DIRS for part in rel.parts):
            continue
        out[rel.as_posix()] = f.read_bytes()
    return out


def pull(project, root, rev=0):
    co = json.loads(call("source_checkout", {"project": project, "rev": rev, "max_bytes": 2000000}))
    root.mkdir(parents=True, exist_ok=True)
    missing = [f["path"] for f in co["files"] if "text" not in f]
    contents = {f["path"]: f["text"].encode() for f in co["files"] if "text" in f}
    for i in range(0, len(missing), 50):
        for f in json.loads(call("source_read", {"project": project, "rev": co["rev"], "paths": missing[i:i + 50]})):
            contents[f["path"]] = f["text"].encode() if "text" in f else base64.b64decode(f["base64"])
    for rel in local_files(root):
        if rel not in contents:
            (root / rel).unlink()
    for rel, data in contents.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(data)
    (root / MARK).write_text(json.dumps({"project": project, "rev": co["rev"]}) + "\n")
    print(f"{project} revision {co['rev']}: {len(contents)} files in {root}")


def source_cmd(args):
    import hashlib
    if not args or args[0] == "list":
        for p in json.loads(call("source_list", {})):
            print(f"{p['project']:24} head {p['head']:<5} deployed {p['deployed_rev'] or '-':<5} {p['description']}")
    elif args[0] == "new":
        project, root = args[1], pathlib.Path(args[2])
        call("source_create", {"project": project, "template": args[3] if len(args) > 3 else "empty"})
        pull(project, root)
    elif args[0] == "pull":
        pull(args[1], pathlib.Path(args[2]), int(args[3]) if len(args) > 3 else 0)
    elif args[0] == "push":
        root, message = pathlib.Path(args[1]), args[2]
        mark = json.loads((root / MARK).read_text())
        co = json.loads(call("source_checkout", {"project": mark["project"], "rev": mark["rev"], "max_bytes": 0}))
        theirs = {f["path"]: f["sha256"] for f in co["files"]}
        mine = local_files(root)
        changes = []
        for rel, data in mine.items():
            if theirs.get(rel) != hashlib.sha256(data).hexdigest():
                try:
                    changes.append({"path": rel, "text": data.decode("utf-8")})
                except UnicodeDecodeError:
                    changes.append({"path": rel, "base64": base64.b64encode(data).decode()})
        changes += [{"path": rel, "delete": True} for rel in theirs if rel not in mine]
        if not changes:
            print("nothing to push")
            return
        r = json.loads(call("source_commit", {"project": mark["project"], "base_rev": mark["rev"], "message": message,
                                              "changes": changes}))
        if not r.get("committed"):
            sys.exit(f"refused: {r.get('error')}\nconflicts: {', '.join(r.get('conflicts', []))}; pull again and redo them")
        mark["rev"] = r["rev"]
        (root / MARK).write_text(json.dumps(mark) + "\n")
        print(f"{mark['project']} revision {r['rev']}: {len(changes)} changes" + (" (on top of others' work)" if r["rebased"] else ""))
    elif args[0] in ("build", "deploy"):
        r = json.loads(call("source_" + args[0], {"project": args[1], "rev": int(args[2]) if len(args) > 2 else 0}))
        while r["state"] in ("queued", "running"):
            r = json.loads(call("source_job", {"job": r["job"]}))
        print(r["log"], end="")
        if r["state"] != "succeeded":
            sys.exit(1)
    else:
        sys.exit("source list | new | pull | push | build | deploy")


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
    elif cmd == "source":
        source_cmd(argv[2:])
    else:
        sys.exit(f"unknown command {cmd}; try --help")


if __name__ == "__main__":
    main(sys.argv)
