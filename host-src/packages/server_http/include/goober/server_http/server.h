#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goober::server::http {

struct Request {
  std::string method;  // lowercase: get, head, post, ...
  std::string target;  // as sent: path plus query
  std::string path;    // the raw path, before normalization
  std::string query;   // the raw query, without "?"
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercase
  std::string body;
  std::string remote;  // the peer address

  // The first header with this lowercase name, or "".
  std::string header(std::string_view name) const;
};

// A response that stays open (server-sent events, design/machine/036.Bus.md § 4). Safe to use
// from any thread.
class Stream {
 public:
  virtual ~Stream() = default;
  // Queues bytes to send; false once the stream is closed (by either side, or for falling more
  // than the response's stream_max_behind bytes behind).
  virtual bool send(std::string bytes) = 0;
  virtual void close() = 0;
  virtual bool closed() const = 0;
  // Called once when the stream closes, for whatever reason; at once if it already has.
  virtual void on_close(std::function<void()> f) = 0;
};

struct Response {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool head_only = false;  // send the headers and length, but no body
  // A host-generated error page, which the host may re-render (as JSON, for an API
  // caller) before it is sent. The server ignores it.
  bool host_error = false;
  // Set: the status and headers go out with no length and "connection: close", the body is
  // ignored, and this is called with the open stream (on a network thread; it must not block).
  std::function<void(std::shared_ptr<Stream>)> stream;
  size_t stream_max_behind = 256 * 1024;
};

// Called exactly once per request, from any thread.
using Responder = std::function<void(Response)>;

// Runs on a network thread; must not block. Long work goes to another pool,
// which calls the responder when done.
using Handler = std::function<void(Request, Responder)>;

struct ServerOptions {
  std::string address = "127.0.0.1";
  uint16_t port = 8080;  // 0 picks a free port
  int threads = 2;
  uint64_t body_max_bytes = 1 << 20;
  // Once a request's headers are read, may raise its body limit (an operator uploading a
  // site or a box, say); returns 0 to keep body_max_bytes. Runs on a network thread.
  std::function<uint64_t(std::string_view target, std::string_view authorization)> body_limit_for;
  int idle_timeout_seconds = 30;
  // Connections open at once; past it, new ones are closed at accept, so a flood of idle
  // sockets can't hold the host's memory and descriptors.
  int max_connections = 4096;
};

// An HTTP/1.1 server on Boost.Beast with keep-alive, a body limit, and an idle timeout.
class Server {
 public:
  Server(ServerOptions options, Handler handler);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds and starts the network threads. Throws on a bind failure.
  void start();
  void stop();
  // Closes the listening socket; connections already open keep being served.
  void stop_accepting();
  // Connections open right now.
  int open_connections() const;
  uint16_t port() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace goober::server::http
