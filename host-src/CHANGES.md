# What's new on a goober host

Served by the `whats_new` tool, built into the binary: each entry is something agents and
engineers can do now, newest first, with the tools to use. A host has every entry up to its
build (`host_health`'s `build`). Add an entry with every change agents should know about.

## 2026-10-31

- Error pages: content_put {tenant, path: "error-message/404.html", text} (or 403, 500, ...) is the page a browser gets when the host answers that status. It is a template ({{status}}, {{reason}}, {{request_id}}, the site's data), "layout: base" front matter wraps it in the site's layout, and partials work; it renders for a visitor with no session. /error-message/... itself answers 404 and no sitemap lists it; page_put refuses that path. site_guide's "Error pages" lists the statuses.
- A box route's 4xx with an empty body gets the site's error page for it (an API caller gets the host's JSON); a 4xx with a body keeps the box's body.
- error_report {window, tenant?}: every 4xx and 5xx visitors and clients got, against all requests, by status and by place (a box route, a page, a host area, or "(not found)" for every path nothing answers), split by error_by (host, box, box_failed) and by whether a link on this site, another site, or none led there. The panel's Errors view.
- box_failures {window, tenant?, box?, latest?}: every box call (requests, tools, timers, mail, jobs, internal calls) and how it failed, by class (trap, timeout, out_of_fuel, failed, bad_response, bad_status, render_failed, commit_failed, ...; sql_error, no_clock, refused_write per statement; delivery_retry, delivery_dead; internal_failed; mail_rejected, mail_deferred), per box and route, with the latest messages and request ids. The panel's Box failures view.
- The sitemap's icicle (uri_tree) holds the site's own traffic: requests the host refused before any box ran ("(refused)") and paths nothing answers ("(not found)") stay out; a box route's own errors stay on it. Access lines say error_by and ref (same or other site).

- route_try reaches MCP tools ({tool, arguments, as: {email}}) and mail routes ({mail: {to, from, subject, text, headers?}}, answered with the SMTP reply), always on copies; a try's mail sender is never verified. Observers and the panel may try HTTP GETs and HEADs only.
- version_health reads every access-log file still on the machine (7 days) for its window, not only the newest two, and leaves out route_try's calls.
- partial_put, partial_delete, and content_put take if_etag (the file_etag you read, or "none" for a new one): no write lands over a teammate's change made since you read it. Every content write now can.
- Every version_upload (live, activate: false, dry_run) answers review and widens: each change that adds reach (a grant, a write, a route made public, a new mail address, service, From address, domain, browser caching, flag value), in sentences; a person approving a held upload sees them. version_diff {box, from?, to?} compares stored versions; version_list shows stored_at.
- version_health {box, minutes?}: after a version goes live, compares each route's failures and latency with the version it replaced, and names the version_promote that rolls back. Nothing rolls back by itself.
- content_deploy {dry_run: true} runs every check, renders each URL signed out from the would-be files, lints it, and answers status, body_etag, and problems; nothing is stored, and it never waits for approval.
- content_stage builds one content deploy over several calls (kept an hour); content_deploy {tenant, stage: true} deploys the whole draft at once, with every check first.
- page_lint {body, path, format} lints an unsaved page; page_render {as: "signed_out"} shows what a visitor gets (a gated page's 303 or 401); content_deploy items take expect_status and expect_location, verified signed out.
- Title and description lengths count characters, not bytes (title_chars).
- route_try: call any stored version's HTTP route (dark ones too) as signed_out or as an account ({email}), before version_promote. GETs read live data read-only; writes run on copies and report the rows they changed and what they would have queued. Nothing reaches people or real data.
- whats_new: this list, by day; call it after a deploy or a reconnect to learn what arrived.
- A real job_start snapshots every database the box may write before its first batch (the run's snapshots name them); db_restore_to_new brings the data back as it was.
- A deep link's #fragment survives signing in (/docs/guide#install lands on #install after the code).
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
