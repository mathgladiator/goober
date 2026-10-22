# host

Wires every package into the running host: request flow (operations, box routes, static
content, error pages), the box worker pool, the operator MCP tools, access and box logs,
and the `goober-host` binary with SIGHUP reload.

## Quality bar

- End-to-end tests run a real host on a free port and drive it only through HTTP and MCP,
  the way an agent would.
- Network threads only route. Box calls, static reads, and operator tools run on the
  worker pool.
- Every response carries a `goober-request-id` and gets one access-log line. A box's
  failures land in its log with the outcome and the reason, but never in the page a
  visitor sees.
- Credentials (`authorization`, `cookie`) never reach a box. A box learns who is calling
  only from `user` in its request document.
- Every box route is gated by its requirements before the box runs: 303 to sign in for a
  browser, 401 otherwise, 403 for unmet requirements, 400 for smuggled `/` or `:`.
- Identity pages are host-owned and unframeable, and refuse cross-origin POSTs.
- Operator tokens are stored only as hashes and checked per request, so revoking is immediate.
