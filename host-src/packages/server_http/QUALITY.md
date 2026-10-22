# server_http

The HTTP/1.1 server on Boost.Beast: network threads that read requests, hand them to a
handler, and write responses posted back from any thread.

## Quality bar

- Network threads never run box code. The handler must return quickly; the responder may
  be called later, from any thread, exactly once (extra calls are ignored).
- The body limit and the idle timeout are always on, so a slow or huge client costs at most
  one connection's buffers.
- This package knows nothing about goober: no routing, no tenants, no boxes. It turns
  bytes into `Request` and `Response` into bytes.
