#!/usr/bin/env python3
"""Plays the two example products end to end against a running goober host (READINESS.md).

    tools/products_e2e.py --config demo/host.json [--url http://localhost:8080]

leads: a visitor from an ad fills in the landing page's form; an agent (GrokBot, with a
minted long-lived token, as Grok's remote MCP would hold one) counts, tallies, and exports.
ranches: the agent adds a ranch whose website carries schema.org data, the host's robot reads
it, the agent logs an outreach, and anyone reads the public index.

It signs in as the tenant's first owner through the dev mailer, so it needs the host's
storage on this machine (local mode). It mints a temporary operator token and revokes it.
The ranch's "website" is a page it puts on this host and removes afterwards. Standard
library only.
"""
import argparse
import json
import os
import secrets
import subprocess
import sys
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from terminal_e2e import Client, check, json_of, last_mail, newest_code, FAILURES  # noqa: E402

RANCH_PAGE = """<!doctype html><html><head><title>E2E Test Ranch</title>
<script type="application/ld+json">{"@context":"https://schema.org","@graph":[
 {"@type":"LocalBusiness","name":"E2E Test Ranch","telephone":"555-0199",
  "address":{"@type":"PostalAddress","addressLocality":"Fresno","addressRegion":"CA"}},
 {"@type":"Product","name":"Brisket (whole packer)","offers":{"@type":"Offer","price":"11.50","priceCurrency":"USD",
  "availability":"https://schema.org/InStock"}},
 {"@type":"Product","name":"Quarter beef share","offers":{"@type":"Offer","price":"950","priceCurrency":"USD",
  "availability":"https://schema.org/OutOfStock"}}]}
</script></head><body><h1>E2E Test Ranch</h1></body></html>"""


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--config", required=True)
    p.add_argument("--url", default="http://localhost:8080")
    p.add_argument("--bin", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "dev",
                                                 "packages", "host", "goober-host"))
    a = p.parse_args()
    cfg = json.load(open(a.config))
    storage = os.path.join(os.path.dirname(os.path.abspath(a.config)), cfg.get("storage", "storage"))
    domain, entry = next((d, e) for d, e in cfg["domains"].items() if e.get("owner"))
    email, tenant = entry["owner"], entry["tenant"]
    web = Client(a.url)
    same_site = {"Origin": web.origin}
    run = secrets.token_hex(3)

    op_name = "products-e2e-" + run
    op_token = subprocess.run([a.bin, "token", "create", "--config", a.config, "--name", op_name, "--local"],
                              capture_output=True, text=True, check=True).stdout.strip()

    def op(tool, args):
        req = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}}
        st, _, body = web.call("POST", "/goober-operations-mcp", js=req, headers={"Authorization": "Bearer " + op_token})
        res = json_of(body).get("result", {})
        text = res.get("content", [{}])[0].get("text", "")
        return (not res.get("isError")), json_of(text) if text.startswith(("{", "[")) else text

    try:
        print("the person exists (signs in once), and GrokBot gets a token")
        before = last_mail(storage)
        web.call("POST", "/identity/email/start", form={"email": email}, headers=same_site)
        st, h, _ = web.call("POST", "/identity/email/verify", form={"email": email, "code": newest_code(storage, before)},
                            headers=same_site)
        check(st == 303, "the owner signs in")
        ok, _ = op("client_put", {"tenant": tenant, "client_id": "grokbot", "name": "GrokBot", "redirect_uris": [],
                                  "acts_as": "agent"})
        check(ok, "an agent client for GrokBot")
        ok, minted = op("agent_token_mint", {"tenant": tenant, "client_id": "grokbot", "email": email, "days": 1})
        check(ok and minted.get("access_token", "").startswith("goa_"), "a minted token for a fixed bearer header")
        agent = {"Authorization": "Bearer " + (minted.get("access_token", "") if ok else "")}

        def tool(name, args):
            req = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args}}
            st, _, body = web.call("POST", "/mcp", js=req, headers=agent)
            res = json_of(body).get("result", {})
            return st, (not res.get("isError")), res.get("structuredContent") or json_of(
                res.get("content", [{}])[0].get("text", "{}"))

        st, _, body = web.call("POST", "/mcp", js={"jsonrpc": "2.0", "id": 1, "method": "tools/list"}, headers=agent)
        names = {t["name"] for t in json_of(body).get("result", {}).get("tools", [])}
        check({"leads_list", "leads_stats", "survey_summary", "campaign_open", "ranch_add", "offering_search"} <= names,
              "the agent sees the products' tools on /mcp")

        print("leads: visitors from an ad")
        campaign = "e2e-" + run
        visitor = f"visitor-{run}@example.com"
        form = {"email": visitor, "consent": "yes", "q_share": "half", "q_zip": "93701", "utm_source": "facebook",
                "utm_content": "ad-b", "next": "/thanks"}
        st, _, page = web.call("POST", f"/leads/{campaign}/signup", form=form, headers=same_site)
        check(st == 400 and "isn't one this site runs" in page, "a campaign nobody opened takes no signups")
        st, ok, out = tool("campaign_open", {"campaign": campaign, "title": "the e2e ad"})
        check(ok and out.get("form_action") == f"/leads/{campaign}/signup", "the agent opens the campaign")
        st, h, _ = web.call("POST", f"/leads/{campaign}/signup", form=form, headers=same_site)
        check(st == 303 and h.get("location") == "/thanks", "the form posts and lands on /thanks")
        st, h, _ = web.call("POST", f"/leads/{campaign}/signup", form={**form, "q_share": "quarter", "q_when": "now"},
                            headers=same_site)
        check(st == 303, "coming back updates the same lead")
        st, _, page = web.call("POST", f"/leads/{campaign}/signup", form={"email": "not-an-email", "consent": "yes"},
                               headers=same_site)
        check(st == 400 and "valid email" in page, "a bad email gets a page to go back from")
        st, _, _ = web.call("POST", f"/leads/{campaign}/signup", form={"email": "x@example.com"}, headers=same_site)
        check(st == 400, "no consent, no signup")
        st, h, _ = web.call("POST", f"/leads/{campaign}/signup",
                            form={"email": f"bot-{run}@example.com", "consent": "yes", "website": "http://spam"},
                            headers=same_site)
        check(st == 303, "a bot filling the honeypot is thanked")
        st, _, _ = web.call("POST", f"/leads/{campaign}/signup", js={"email": f"api-{run}@example.com", "consent": "yes",
                                                                    "q_share": "whole", "utm_source": "google"})
        check(st == 201, "a JSON signup works too")
        st, h, _ = web.call("POST", f"/leads/{campaign}/signup", form=form, headers={"Origin": "https://evil.example"})
        check(st == 403, "a form posted from another site is refused")

        st, ok, out = tool("leads_list", {"campaign": campaign})
        leads = out.get("leads", [])
        mine = next((l for l in leads if l["email"] == visitor), {})
        check(ok and len(leads) == 2, "two leads: the honeypot bot isn't one")
        check(mine.get("answers") == {"share": "quarter", "zip": "93701", "when": "now"} and mine.get("utm_content") == "ad-b",
              "answers merged, the ad remembered")
        st, ok, out = tool("survey_summary", {"campaign": campaign, "question": "share"})
        tally = {x["answer"]: x["count"] for x in out.get("answers", [])}
        check(ok and tally == {"quarter": 1, "whole": 1}, "the survey tallies")
        st, ok, out = tool("leads_stats", {"campaign": campaign})
        srcs = {(x["utm_source"], x["utm_content"]): x["leads"] for x in out.get("sources", [])}
        check(ok and srcs.get(("facebook", "ad-b")) == 1 and out["campaigns"][0]["leads"] == 2, "counts per ad")
        st, ok, out = tool("leads_export", {"campaign": campaign})
        csv = out.get("csv", "")
        check(ok and csv.startswith("email,name,confirmed") and visitor in csv and "q_share" in csv, "a CSV export")
        st, h, body = web.call("GET", f"/leads/v1/export/{campaign}", headers=agent)
        check(st == 200 and h.get("content-type", "").startswith("text/csv") and visitor in body, "CSV over HTTP")
        st, _, _ = web.call("GET", f"/leads/v1/export/{campaign}", headers={"Accept": "application/json"})
        check(st == 401, "the export needs a token")

        print("ranches: the robot reads a ranch's website")
        ok, _ = op("content_put", {"tenant": tenant, "path": f"e2e-ranch-{run}.html", "text": RANCH_PAGE})
        check(ok, "a ranch website to read")
        slug = "e2e-ranch-" + run
        st, ok, out = tool("ranch_add", {"slug": slug, "name": "E2E Test Ranch", "website": f"{web.origin}/e2e-ranch-{run}.html",
                                         "region": "Central Valley", "notes": "met at the farmers market"})
        check(ok and out.get("slug") == slug, "the agent adds a ranch")
        st, ok, out = tool("ranch_add", {"slug": slug, "name": "E2E Test Ranch"})
        check(not ok, "adding it twice is refused")
        st, ok, out = tool("ranch_check", {"slug": slug})
        check(ok and out.get("offerings") == 2, f"the robot found two offerings ({out})")
        st, ok, out = tool("offering_search", {"query": "brisket"})
        hit = next((o for o in out.get("offerings", []) if o["ranch_slug"] == slug), {})
        check(hit.get("price_cents") == 1150 and hit.get("category") == "beef" and hit.get("in_stock") is True,
              "brisket at $11.50, beef, in stock")
        st, ok, out = tool("offering_search", {"query": "quarter", "in_stock": True})
        check(not any(o["ranch_slug"] == slug for o in out.get("offerings", [])), "out of stock is filtered out")
        st, ok, out = tool("contact_log", {"slug": slug, "channel": "email", "direction": "out",
                                           "summary": "Asked about fall quarter shares."})
        check(ok, "the agent logs an outreach")
        st, ok, out = tool("ranch_get", {"slug": slug})
        check(ok and out.get("status") == "contacted" and out.get("phone") == "555-0199" and out.get("city") == "Fresno"
              and len(out.get("contacts", [])) == 1 and out["contacts"][0]["by"] == "grokbot",
              "status, details from the site, and who reached out")
        st, _, body = web.call("GET", "/meat/offerings?q=brisket", headers={"Origin": "https://app.example"})
        pub = json_of(body).get("offerings", [])
        check(st == 200 and any(o["ranch_slug"] == slug for o in pub), "the public index has it")
        check("notes" not in body and "met at the farmers market" not in body, "notes stay private")
        st, _, body = web.call("GET", "/meat/ranches")
        check(st == 200 and slug in body and "555-0199" not in body, "the public directory, without phones")
        op("content_delete", {"tenant": tenant, "path": f"e2e-ranch-{run}.html"})
        tool("ranch_update", {"slug": slug, "status": "declined", "watch": False})
    finally:
        subprocess.run([a.bin, "token", "revoke", op_name, "--config", a.config], capture_output=True)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} failed")
        sys.exit(1)
    print("all products checks passed")


if __name__ == "__main__":
    main()
