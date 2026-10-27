#pragma once

// The text of the box_guide operator tool: everything an agent needs to build and deploy a
// product as boxes on this host, kept in step with the code (design/machine/005 to 014).

namespace goober::host {

inline constexpr const char* kBoxGuide = R"guide(# Building a product on goober

A product is one or more boxes. A box is a WebAssembly module plus a manifest (and, when its
routes take or return typed JSON, a types document). The manifest is the perimeter: the
host serves only the routes it declares, checks every caller against their requirements,
lets the box touch only the tables and columns it grants, and gives it nothing else: no
files, no network, no clock but the host's. Deploy with version_upload; under this host's
dev policy a version that passes the checks goes live at once (version_promote rolls back).

## Deploying

version_upload {manifest, wat | wasm_base64, types?}
- manifest: the JSON object below.
- wat: WebAssembly text, for small boxes; or wasm_base64: a compiled module (Rust with
  `cargo build --release --target wasm32-unknown-unknown`).
- types: the OpenAPI document the manifest's "types" names, when it names one.
Store database schemas first with schema_put. Then check box_logs, access_log, and db_query.

## The boundary (goober-abi@1)

Import only from module "goober":

| import | signature | does |
| --- | --- | --- |
| goober_request_size | () -> i32 | Bytes in the request document. |
| goober_request_read | (ptr, len) -> i32 | Copies the request document into memory. |
| goober_respond | (status, type_ptr, type_len, body_ptr, body_len) -> i32 | Sets the response: status 200..599, a content type, a body up to 16 MiB. The last call wins. |
| goober_header_set | (name_ptr, name_len, value_ptr, value_len) -> i32 | Adds a header; set-cookie, content-type, content-length, and goober-* are the host's. |
| goober_now_ms | () -> i64 | The request's time, fixed for the whole request. |
| goober_random_fill | (ptr, len) -> i32 | Random bytes from a per-request seed (logged, so replayable). |
| goober_log | (level, ptr, len) -> i32 | A line for box_logs, written after the call. |
| goober_sql | (strategy_ptr, strategy_len, sql_ptr, sql_len, params_ptr, params_len) -> i32 | Runs one SQL statement through a data strategy; returns the result's length. |
| goober_result_read | (ptr, len) -> i32 | Copies the last SQL or call result into memory. |
| goober_call | (service_ptr, service_len, method_ptr, method_len, request_ptr, request_len) -> i32 | Calls a declared external service; returns the result's length. |

Export `memory`, and one function per route, tool, and timer: `goober_route_$name`, taking
nothing and returning i32 (0 means it ran; anything else is a 500 and rolls back). A bad
pointer gets -1, never a crash. Each call gets a fresh instance: nothing survives between
requests except what you write to the database.

The request document is JSON:
{"request_id", "method", "path", "params": {path parameters, int64 as strings}, "query",
 "headers", "body": text, "domain", "tenant", "user": null or {"user_id", "email", "roles",
 "host", "agent", "client_id" (agents only)}, "client_key", "form" (forms only)}
- An HTTP route: method get/post/put/patch/delete. Credentials (cookie, authorization) never appear.
- "form": a plain HTML form's fields (application/x-www-form-urlencoded), decoded, on routes
  without a json body type. One value per name (the last).
- "client_key": 16 hex characters, the same for the same address on the same site, so a box
  can spot repeats (one signup per visitor, say) without ever holding an IP address.
- Anonymous POSTs are limited per address (20 a minute, bursts of 10), on top of the host's
  overall limit, so a public form can't become a flood.
- An MCP tool: method "tool", path "mcp:$name", body = the checked input as JSON.
- A timer: method "timer", path "timer:$name", body = the row as JSON (data-driven) or "",
  user null. A timer may simply return without responding.

## The manifest

{
  "box": "orders",                          [a-z][a-z0-9_]*, unique on the host
  "mission": "One sentence a person reads before approving.",
  "domains": ["*"],                         or specific domains
  "behavior": {"timeout-ms": 1000, "fuel": 100000000, "memory-max-bytes": 33554432},
  "types": "orders.openapi.json",           optional; passed to version_upload as types
  "data": {"sqlite3": {"$strategy": {...}}},
  "templates": {"$name": "Mustache HTML"},  renders a route's JSON for browsers
  "routes": {"http": {...}, "mcp": {...}, "timer": {...}}
}
Route, tool, and timer names are [a-z][a-z0-9_]*, unique across the whole host.

### HTTP routes
"routes": {"http": {"/orders/{order_id : int64}": {"get": {
  "name": "get_order",
  "requirements": [["owner"], ["admin"]],   or "public": true (exactly one of the two)
  "agents": true,                           let agent tokens call it (default: people only)
  "query": {"include": {"type": "boolean", "default": false}},   only these reach the box, typed
  "request-headers": ["idempotency-key"],   only these reach the box
  "body": "json:OrderUpdate",               or "text" / "binary"; checked before the box wakes
  "max-body-bytes": 65536,
  "response": "json:Order",                 a 2xx JSON answer must match, or it's a 500
  "template": "order",                      browsers asking for HTML get the JSON rendered
  "cors": ["https://app.example.com"]       browser pages there may call it with a bearer token
}}}}
Answers: 2xx and 4xx as you send them; 5xx is the host's (your body is replaced and your writes
roll back); a redirect (301/302/303/307/308) must point at a path on this site, like
Location: /thanks?ok=1 (an absolute URL, even to this site, is a 500); "response":
"empty" makes any 2xx a 204. GET answers get an ETag and a 304 when the caller has the bytes.
Parameters: {name : string|int32|int64}; literal segments win over parameters.
Requirements are an OR of AND clauses of roles: [] is any signed-in user; "owner",
"admin/project_{project_id}" (a path parameter), "self/user_{!user_id}", and "host:owner"
(host operators only) are all roles. A signed-out browser is sent to sign in; an API caller
gets 401. A POST from another site is refused.

### MCP tools (for agents)
"routes": {"mcp": {"add_order": {"tool": {
  "description": "Adds an order. A model reads this, so say exactly what it does.",
  "input": "json:OrderInput", "requirements": [["owner"]]}}}}
Served at /mcp on every domain to people and agents signed in with OAuth. tools/list shows
only what the caller's roles allow. Inputs are checked against the type; a 2xx JSON object
comes back as structured content, a 4xx as a tool error with your "error" message.

### Timers
"routes": {"timer": {
  "sweep": {"name": "sweep", "every-ms": 60000},
  "nightly": {"name": "nightly", "cron": "0 3 * * *"},             host's zone
  "digest": {"name": "digest", "source": "people_t", "cron": "0 6 * * *"},   each row's zone
  "remind": {"name": "remind", "source": "reminders_t", "at-field": "remind_at"}}}
A data-driven timer fires once per active row of a by_timer strategy; an at-field timer fires
once at the row's instant (Unix ms, or RFC 3339 UTC) and the host then sets the row's
active-field to 0 in the same transaction. Try one now with timer_run; see the plan with
timers_list. Fires run once per tenant the box serves.

## Data

1. schema_put {"schema": "orders1", "tables": {"orders": {"columns": {
     "id": {"type": "int64", "primary-key": true},
     "title": {"type": "string", "max-length": 200},
     "qty": {"type": "int32", "default": 1},
     "paid": {"type": "boolean", "default": false},
     "note": {"type": "string", "nullable": true}},
     "indexes": {"orders_by_title": {"columns": ["title"], "unique": false}}}}}
   Types: int32, int64, double, float, boolean, string. A table gets "id" if it names no
   primary key. A schema name is a version: never reused. Later schemas may only add tables,
   nullable or defaulted columns, and indexes.
2. In the manifest:
   "data": {"sqlite3": {"db": {"schema": "orders1", "database": "orders", "pattern": "sql",
     "tables": {"orders": {"read": true, "insert": true, "update": ["paid"], "delete": false}},
     "row-limit": 500}}}
   Each tenant gets its own file for "orders". Grants are per table and column; nothing else
   can be read or written. Two boxes may share a database with different grants: give an
   agents' box insert-only access and keep the deciding writes in a box people call.
   "tenant": "shared" (any name) makes one database for every domain and box that names it,
   instead of one per tenant: use it for data that spans sites, like a public index. Boxes
   sharing a database must bring schemas that fit together; upload checks.
   by_timer strategies: {"schema", "database", "pattern": "by_timer", "table",
   "timezone-field"?, "active-field"?} are read only by the scheduler.
3. goober_sql(strategy, sql, params): params is a JSON array for ? or an object for :name.
   The result is {"columns", "rows", "changes", "last_insert_id"} or {"error"}. Let SQLite
   build your JSON: SELECT json_object(...), json_group_array(...) answers in one statement.
   One statement per call; no PRAGMA, ATTACH, DDL, transactions, or REPLACE (use
   INSERT ... ON CONFLICT DO UPDATE); pure functions only; 2 s, 4 MiB per value, 8 MiB per
   result. All of a request's writes commit together if the box answers below 500.
   Operators look with db_list, db_describe, and db_query.

## Calling other services

A box has no sockets. It calls services the manifest declares, through the host:
"external": {"stripe": {
  "base": "https://api.stripe.com",            https (or http://127.0.0.1:PORT, local mode only)
  "credentials": "keys/system/stripe",         or keys/tenant/{!tenant}/stripe; set with key_set
  "auth": {"header": "Authorization", "prefix": "Bearer "},   the default
  "methods": {
    "price": {"verb": "get", "path": "/v1/prices/{price_id}"},             safe: sent now
    "refund": {"verb": "post", "path": "/v1/refunds", "idempotency": "header/Idempotency-Key",
               "require": {"body.metadata.tenant": "{!tenant}"},
               "delivery": {"attempts": 8, "backoff-ms": 1000, "within-ms": 600000},
               "budget": {"calls": 3}}}}}
goober_call("stripe", "price", {"path": {"price_id": "p_1"}, "query": {...}}) answers
{"status", "content_type", "body", "json"} at once. An effectful method (post, put, patch,
delete; or "effect": "outbox") answers {"accepted": true, "key"} and is delivered only if the
request commits, retried with the same idempotency key, and dead-lettered if it can't land
(outbox_dead_letters, outbox_retry). "effect": "immediate" sends an effectful call now, even if
the request later fails. require pins fields to {!email}, {!user_id}, {!tenant}, {!domain}, or a
literal, overriding whatever the box sent. The box never sees the key; logs/egress.log records
every call without query strings or bodies.

## Calling other boxes

A box offers internal routes, reachable only from the boxes it names:
"routes": {"internal": {"/jobs/{job_id : int64}/invoices": {"post": {
  "name": "create_invoice", "callers": ["jobs"], "requirements": [["owner"]],
  "body": "json:InvoiceRequest"}}}}
The caller declares the call: "external": {"billing": {"box": "billing",
"methods": {"create_invoice": {"budget": {"calls": 2}}}}}. Both sides must agree when either is
deployed, and no chain of calls may loop. goober_call("billing", "create_invoice", {"path":
{"job_id": "42"}, "body": {...}}) runs the callee now, as the same person in the same tenant
(its requirements are checked against that person), with what's left of the caller's time,
and answers {"status", "body", "json"}. The callee sees "caller": {"box", "route"} in its
request document. Its writes commit on their own, even if the caller later fails.

## Types

The types document is an OpenAPI 3.x document; only components.schemas is read, in a strict
subset: integer needs format int32 or int64; number needs float or double; strings may have
maxLength, minLength, enum, and format date-time (RFC 3339 UTC) or date; objects need
"additionalProperties": false; arrays need items and maxItems; $ref only to
#/components/schemas/X; no oneOf/anyOf/allOf/null/recursion. Defaults are filled in, and
int64 values reach the box as strings.

## Landing pages and forms

A landing page (an ad's destination, say) is content: a whole .html document with no front
matter is served exactly as written, inline scripts and pixels included (site_guide). Its
<form method="post" action="/signup"> posts to a public box route; the box reads "form",
stores the row (a unique column and ON CONFLICT DO NOTHING make repeats harmless), sends any
confirmation mail through an outbox service, and answers 303 with Location: /thanks. Don't pin
{!email} in require for anonymous visitors: nobody is signed in, so the call would fail.

## Agents that can't do OAuth

An agent that sends one fixed bearer header (Grok's remote MCP tool, a script) gets a token from
agent_token_mint: register it with client_put (acts_as agent), have the person sign in once,
then mint a token for 1 to 366 days. It reaches https://<domain>/mcp and routes with
"agents": true, as that person; the person can revoke it on their account page.

## Writing boxes in Rust

The rust_binding tool returns goober.rs, a ready binding (Request, respond_json, sql, call,
and checks), with the Cargo.toml and build command; it is also
host-src/examples/boxes/shared/goober.rs, used by the terminal and desk boxes. A crate needs only
`crate-type = ["cdylib"]`, `serde_json`, and `#[unsafe(no_mangle)] pub extern "C" fn
goober_route_x() -> i32`. Small boxes can be WAT; see host-src/examples/boxes/echo.

## A good product, briefly

- Write the mission so a person can approve it in one read.
- Give agents their own box and MCP tools, with no write path to what a person must decide.
- Declare query, headers, body, and response types: invalid input never reaches your code.
- Make retries harmless: idempotency keys in a unique column, ON CONFLICT DO NOTHING.
- Pages and listings are content (site_guide); boxes are for what changes.
)guide";

}  // namespace goober::host
