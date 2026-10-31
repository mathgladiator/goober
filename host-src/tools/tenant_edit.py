#!/usr/bin/env python3
"""The deploy's edits for a new tenant (deploy.sh --add-tenant): host.json and the Caddyfile.

Run by server-bootstrap.sh, host.json as the host's user and the Caddyfile as root, each fed on
stdin so neither reads a path the other owns. Tested by tenant_edit_test.py.

  tenant_edit.py inspect HOST_JSON DOMAIN
      JSON: whether DOMAIN can be added, the tenant suggested for it, the tenants there are.
  tenant_edit.py host-json HOST_JSON --domain D --tenant T [--owner E] [--postal P]
                 [--terms PATH] [--privacy PATH] [--www]
      Adds D for T (and www.D redirecting to D). A tenant that exists keeps every setting it
      has; only missing ones are filled.
  tenant_edit.py caddy CADDYFILE --domain D [--www]
      Adds a site block for D (and www.D), a copy of the body of the block that proxies to goober.
  tenant_edit.py dns DOMAIN SITE
      JSON: DOMAIN's and www.DOMAIN's addresses, and whether each points where SITE does.
"""
import argparse
import json
import os
import re
import socket
import sys

DNS = re.compile(r"[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+")
TENANT = re.compile(r"[a-z][a-z0-9_]{0,62}")
EMAIL = re.compile(r"[^@\s]+@[^@\s]+\.[^@\s]+")
PAGE = re.compile(r"/[A-Za-z0-9._~/-]{0,200}")


def dns_name_problem(d):
    if not DNS.fullmatch(d) or len(d) > 253:
        return "isn't a domain name, like example.com"
    if re.fullmatch(r"[0-9.]+", d):
        return "is an address; give a domain name"
    if d == "localhost" or d.endswith(".localhost"):
        return "is a local name; a tenant's domain is public"
    return None


def suggest_tenant(domain):
    label = domain.split(".")[0].replace("-", "_")
    if domain.startswith("www.") and domain.count(".") >= 2:
        label = domain.split(".")[1].replace("-", "_")
    if not label[:1].isalpha():
        label = "t_" + label
    return label[:63]


def inspect(cfg, domain):
    domains = cfg.get("domains", {})
    tenants = sorted({e["tenant"] for e in domains.values() if isinstance(e, dict) and "tenant" in e})
    problem = dns_name_problem(domain)
    if not problem:
        if domain in domains:
            e = domains[domain]
            problem = ("already redirects to " + e["redirect-to"]) if "redirect-to" in e else \
                ("is already served for " + e.get("tenant", "?"))
        elif domain == cfg.get("operations", {}).get("domain"):
            problem = "is the panel's domain"
    www = "www." + domain
    return {
        "ok": problem is None,
        "problem": problem,
        "suggested": suggest_tenant(domain),
        "tenants": tenants,
        "platform": cfg.get("platform-domain", ""),
        # www. is offered for a name like example.com, unless it's already taken.
        "www": not domain.startswith("www.") and www not in domains and www != cfg.get("operations", {}).get("domain"),
    }


def edit_host_json(cfg, a):
    info = inspect(cfg, a.domain)
    if not info["ok"]:
        sys.exit(a.domain + " " + info["problem"])
    if not TENANT.fullmatch(a.tenant):
        sys.exit("a tenant name matches [a-z][a-z0-9_]*, like " + info["suggested"])
    if a.owner and not EMAIL.fullmatch(a.owner):
        sys.exit("the owner must be an email address")
    if a.postal and (len(a.postal) < 5 or len(a.postal) > 200 or "\n" in a.postal or "\r" in a.postal):
        sys.exit("the postal address is one line of 5 to 200 characters")
    for name, value in (("terms", a.terms), ("privacy", a.privacy)):
        if value and not PAGE.fullmatch(value):
            sys.exit(name + " is a path on the site, like /" + name)
    if a.www and not info["www"]:
        sys.exit("www." + a.domain + " is already in host.json")
    entry = {"tenant": a.tenant}
    if a.owner:
        entry["owner"] = a.owner.lower()
    cfg.setdefault("domains", {})[a.domain] = entry
    if a.www:
        cfg["domains"]["www." + a.domain] = {"redirect-to": a.domain}
    tenant = cfg.setdefault("tenants", {}).setdefault(a.tenant, {})
    did = ["domains." + a.domain]
    if a.www:
        did.append("domains.www." + a.domain + " (a redirect)")
    for key, value in (("postal-address", a.postal), ("terms", a.terms), ("privacy", a.privacy)):
        if value and not tenant.get(key):
            tenant[key] = value
            did.append("tenants." + a.tenant + "." + key)
    if not tenant:
        del cfg["tenants"][a.tenant]
        if not cfg["tenants"]:
            del cfg["tenants"]
    return did


def site_names(line):
    names = []
    for addr in line.rstrip()[:-1].replace(",", " ").split():
        addr = addr.split("://", 1)[-1]
        names.append(addr.split(":", 1)[0].split("/", 1)[0].lower())
    return names


def edit_caddyfile(text, domain, www):
    """The Caddyfile with a block for domain (and www.domain), copied from goober's block."""
    lines = text.split("\n")
    blocks = []  # (first line, last line) of each top-level site block
    i = 0
    while i < len(lines):
        s = lines[i].rstrip()
        if s.endswith("{") and len(s) > 1 and s[0] not in " \t#{(":
            depth, j = 0, i
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth <= 0:
                    break
                j += 1
            blocks.append((i, j))
            i = j + 1
            continue
        i += 1
    wanted = [domain] + (["www." + domain] if www else [])
    for first, _ in blocks:
        for n in wanted:
            if n in site_names(lines[first]):
                raise ValueError("the Caddyfile already serves " + n)
    for first, last in blocks:
        body = lines[first + 1:last]
        joined = "\n".join(body)
        if "reverse_proxy" in joined and "health_uri /goober/health" in joined:
            new = [", ".join(wanted) + " {"] + body + ["}"]
            out = lines[:]
            while out and out[-1] == "":
                out.pop()
            return "\n".join(out + [""] + new) + "\n"
    raise ValueError("no site block proxies to goober (health_uri /goober/health) to copy")


def resolve(name):
    try:
        return sorted({a[4][0] for a in socket.getaddrinfo(name, None, proto=socket.IPPROTO_TCP)})
    except OSError:
        return []


def dns_state(theirs, ours):
    if not theirs:
        return "missing"
    if not ours:
        return "unknown"
    return "ready" if all(a in ours for a in theirs) else "elsewhere"


def write_json(path, cfg):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    os.chmod(tmp, os.stat(path).st_mode & 0o777)
    os.replace(tmp, path)


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("inspect")
    i.add_argument("host_json")
    i.add_argument("domain")
    h = sub.add_parser("host-json")
    h.add_argument("host_json")
    c = sub.add_parser("caddy")
    c.add_argument("caddyfile")
    for x in (h, c):
        x.add_argument("--domain", required=True)
        x.add_argument("--www", action="store_true")
    h.add_argument("--tenant", required=True)
    for k in ("--owner", "--postal", "--terms", "--privacy"):
        h.add_argument(k, default="")
    d = sub.add_parser("dns")
    d.add_argument("domain")
    d.add_argument("site")
    a = p.parse_args()
    if a.cmd == "inspect":
        print(json.dumps(inspect(json.load(open(a.host_json)), a.domain.lower().rstrip("."))))
    elif a.cmd == "host-json":
        a.domain = a.domain.lower().rstrip(".")
        cfg = json.load(open(a.host_json))
        did = edit_host_json(cfg, a)
        write_json(a.host_json, cfg)
        print("set " + ", ".join(did))
    elif a.cmd == "caddy":
        domain = a.domain.lower().rstrip(".")
        if dns_name_problem(domain):
            sys.exit(domain + " " + dns_name_problem(domain))
        try:
            text = edit_caddyfile(open(a.caddyfile).read(), domain, a.www)
        except ValueError as e:
            sys.exit(str(e))
        with open(a.caddyfile, "w") as f:
            f.write(text)
    else:
        ours = resolve(a.site)
        out = {"addresses": ours}
        for n in (a.domain, "www." + a.domain):
            theirs = resolve(n)
            out[n] = {"addresses": theirs, "dns": dns_state(theirs, ours)}
        print(json.dumps(out))


if __name__ == "__main__":
    main()
