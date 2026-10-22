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

struct Response {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool head_only = false;  // send the headers and length, but no body
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
  int idle_timeout_seconds = 30;
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
  uint16_t port() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace goober::server::http
