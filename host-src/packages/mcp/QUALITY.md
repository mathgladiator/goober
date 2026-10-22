# mcp

A small MCP server over streamable HTTP: JSON-RPC 2.0 requests in a POST body, answered
with `application/json`. It supports initialize, ping, tools/list, and tools/call; the
host registers the tools.

## Quality bar

- Protocol errors use JSON-RPC codes (-32700, -32600, -32601, -32602). A tool's own failure
  is a result with `isError: true`, so an agent sees the message.
- A tool that throws becomes an error result, never a crashed connection.
- This package knows no goober operations. Authentication and the tools themselves live in
  the host.
