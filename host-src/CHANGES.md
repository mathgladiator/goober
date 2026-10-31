# What's new on a goober host

Served by the `whats_new` tool, built into the binary: each entry is something agents and
engineers can do now, newest first, with the tools to use. A host has every entry up to its
build (`host_health`'s `build`). Add an entry with every change agents should know about.

## 2026-10-31

- Box SQL has no clock: 'now', CURRENT_TIMESTAMP and its kin, and date functions with no value are refused, and the statement is undone; pass the call's now_ms (datetime(:now_ms / 1000, 'unixepoch')). Operators' db_query keeps the clock.
- The id service's first id is 4294967297 (2^32 + 1): every new id is past the 32-bit range; treat ids as 64-bit strings everywhere.
- Mail routes get from_user_id (a string) when the host verifies the sender (a DKIM signature aligned with the From domain) and the address belongs to an active account; otherwise null, with sender_why.
- Route caching, opt-in per GET route: "cache": {"by": "data" | "wake", "fresh-ms", "browser-ms", "check-percent"}; 304s without the box; sampled checks; cache_status, cache_clear.
- Performance reports: perf_report (per-route time split and findings: cacheable, missing_index, lock_wait, get_writes, subscription_churn, cache_mismatch, browser_cache) and perf_statements; the panel's Performance view.
- Every operator tool says whether it is a read tool or a changing tool (waits for approval when an agent's approvals are on), what it returns, and whether it goes live, is stored dark, or stores nothing. The operator server's instructions define the shared terms.
- Box flags: a manifest's "flags" declares switches; boxes read goober_flag(name) (Rust: goober::flag); operators set flag_set {tenant, box, flag, value, reason} and read flag_list. A freeze needs no new version.
- Databases: db_create (from stored schemas, empty), db_snapshot, db_snapshots, db_restore_to_new, db_unbound. Drops happen only in the panel, by a person, with a snapshot first.
- Schemas: "not-a-reference": true on a column silences the reference warning; types accept const; pattern is refused (no safe regex engine yet).
- Moving a route between boxes: a disabled box holds no names. version_upload {activate: false} warns on a clash; box_disable the old box, version_promote the new; roll back the same way in reverse.
- version_upload {dry_run: true, schemas: [...]} checks a manifest against schemas not stored yet.
- Templates take if_etag (template_put, template_delete); template_get and template_list return file_etag; list_etags covers templates and partials.
- Every answer from a templated box route carries Vary: Accept.
- Write tools answer change_id / change_ids (numbers) for the change log rows they made.
- Etags are named everywhere: file_etag (the stored file's, what if_etag compares) and body_etag (the served body's).
- version_upload {dry_run: true} never waits for approval; source_checkout is a read tool; mail_unsubscribed with "set" waits for approval.

## 2026-10-30

- Ids: one id service mints users, invites, uploads, and rows keyed {"type": "id", "primary-key": true}; a box never names a key; ids are strings in JSON; ids_status.
- Jobs: routes.job, one-time boxes an operator starts; job_dry_run on copies first, then job_start; batches with a kept cursor; job_list, job_status, job_stop.
- Account deletion: from /identity/account or user_delete; boxes get routes.identity "deleted" and erase within 30 days; deletions.
- Mail: mail_domain_check (MX, SPF, DMARC, SES DKIM), mail_route_check, the SMTP listener in host_health, test sends in mail_activity.
- Source: source_stage builds one commit over several calls; schema_check; version_upload {activate: false} stores a version dark; box_disable / box_enable.
- Box pages render inside the site's layout (layout: base front matter); box_render previews them; version_upload {dry_run: true}.
- Redirect domains (www. to the site); deploys offer them.
- The panel: source projects, revisions, diffs, build logs; grants down to the table (grant_usage).
- GETs change nothing: a GET or HEAD that tries to write is refused.
