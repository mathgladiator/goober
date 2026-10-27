#include "goober/server_http/server.h"

#include <atomic>
#include <future>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace goober::server::http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
using tcp = asio::ip::tcp;

std::string Request::header(std::string_view name) const {
  for (const auto& [k, v] : headers) {
    if (k == name) return v;
  }
  return "";
}

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(tcp::socket socket, const ServerOptions& options, const Handler& handler, std::atomic<int>& open)
      : stream_(std::move(socket)), options_(options), handler_(handler), open_(open) {
    ++open_;
  }
  ~Session() { --open_; }

  void run() {
    asio::dispatch(stream_.get_executor(), [self = shared_from_this()] { self->read(); });
  }

 private:
  void read() {
    parser_.emplace();
    parser_->body_limit(options_.body_max_bytes);
    parser_->header_limit(64 * 1024);
    stream_.expires_after(std::chrono::seconds(options_.idle_timeout_seconds));
    bhttp::async_read(stream_, buffer_, *parser_,
                      [self = shared_from_this()](beast::error_code ec, size_t) { self->on_read(ec); });
  }

  void on_read(beast::error_code ec) {
    if (ec == bhttp::error::end_of_stream || ec == beast::error::timeout || ec == asio::error::operation_aborted) {
      return close();
    }
    if (ec == bhttp::error::body_limit) return reply_error(413, "Payload Too Large");
    if (ec) return reply_error(400, "Bad Request");

    auto msg = parser_->release();
    keep_alive_ = msg.keep_alive();
    Request req;
    req.method = lower(std::string_view(msg.method_string()));
    req.target = std::string(msg.target());
    auto q = req.target.find('?');
    req.path = req.target.substr(0, q);
    if (q != std::string::npos) req.query = req.target.substr(q + 1);
    for (const auto& field : msg) {
      req.headers.emplace_back(lower(std::string_view(field.name_string())), std::string(field.value()));
    }
    req.body = std::move(msg.body());
    beast::error_code peer_ec;
    auto ep = stream_.socket().remote_endpoint(peer_ec);
    if (!peer_ec) req.remote = ep.address().to_string();

    stream_.expires_never();
    auto self = shared_from_this();
    auto responded = std::make_shared<std::atomic<bool>>(false);
    handler_(std::move(req), [self, responded](Response res) {
      if (responded->exchange(true)) return;
      asio::post(self->stream_.get_executor(),
                 [self, res = std::move(res)]() mutable { self->write(std::move(res)); });
    });
  }

  void reply_error(int status, const char* text) {
    keep_alive_ = false;
    Response res;
    res.status = status;
    res.headers.emplace_back("content-type", "text/plain; charset=utf-8");
    res.body = std::string(text) + "\n";
    write(std::move(res));
  }

  // Never lets an exception out onto the network thread, where it would end the process:
  // a response that can't be framed becomes a plain 500.
  void write(Response res) {
    try {
      frame_and_send(std::move(res));
    } catch (const std::exception&) {
      keep_alive_ = false;
      Response fallback;
      fallback.status = 500;
      fallback.headers.emplace_back("content-type", "text/plain; charset=utf-8");
      fallback.body = "Internal Server Error\n";
      frame_and_send(std::move(fallback));
    }
  }

  void frame_and_send(Response res) {
    // No header value may carry CR, LF, or NUL, wherever it came from: no response splitting.
    for (const auto& [k, v] : res.headers) {
      if (k.find_first_of("\r\n\0:", 0, 4) != std::string::npos || v.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
        throw std::invalid_argument("a header holds a control character");
      }
    }
    auto status = static_cast<bhttp::status>(res.status);
    std::optional<bhttp::message_generator> gen;
    if (res.head_only) {
      bhttp::response<bhttp::empty_body> out(status, 11);
      for (const auto& [k, v] : res.headers) out.insert(k, v);
      out.set(bhttp::field::server, "goober");
      out.content_length(res.body.size());
      out.keep_alive(keep_alive_);
      gen.emplace(std::move(out));
    } else {
      // 1xx, 204, and 304 carry no body; Beast refuses to frame one, and so do we.
      unsigned code = unsigned(res.status);
      if (code < 200 || code == 204 || code == 304) res.body.clear();
      bhttp::response<bhttp::string_body> out(status, 11);
      for (const auto& [k, v] : res.headers) out.insert(k, v);
      out.set(bhttp::field::server, "goober");
      out.body() = std::move(res.body);
      out.prepare_payload();
      out.keep_alive(keep_alive_);
      gen.emplace(std::move(out));
    }
    stream_.expires_after(std::chrono::seconds(options_.idle_timeout_seconds));
    beast::async_write(stream_, std::move(*gen), [self = shared_from_this()](beast::error_code ec, size_t) {
      if (ec || !self->keep_alive_) return self->close();
      self->read();
    });
  }

  void close() {
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
  }

  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  std::optional<bhttp::request_parser<bhttp::string_body>> parser_;
  const ServerOptions& options_;
  const Handler& handler_;
  std::atomic<int>& open_;
  bool keep_alive_ = false;
};

}  // namespace

struct Server::Impl {
  Impl(ServerOptions o, Handler h) : options(std::move(o)), handler(std::move(h)), acceptor(io) {}

  void accept() {
    acceptor.async_accept(asio::make_strand(io), [this](beast::error_code ec, tcp::socket socket) {
      if (ec) {
        if (ec == asio::error::operation_aborted) return;
      } else {
        if (open.load() >= options.max_connections) {
          beast::error_code ignored;
          socket.close(ignored);
        } else {
          std::make_shared<Session>(std::move(socket), options, handler, open)->run();
        }
      }
      accept();
    });
  }

  ServerOptions options;
  std::atomic<int> open{0};
  Handler handler;
  asio::io_context io;
  tcp::acceptor acceptor;
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
  std::vector<std::thread> threads;
  uint16_t bound_port = 0;
};

Server::Server(ServerOptions options, Handler handler)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(handler))) {}

Server::~Server() { stop(); }

void Server::start() {
  auto& im = *impl_;
  tcp::endpoint ep(asio::ip::make_address(im.options.address), im.options.port);
  im.acceptor.open(ep.protocol());
  im.acceptor.set_option(asio::socket_base::reuse_address(true));
  im.acceptor.bind(ep);
  im.acceptor.listen(asio::socket_base::max_listen_connections);
  im.bound_port = im.acceptor.local_endpoint().port();
  im.work.emplace(im.io.get_executor());
  im.accept();
  for (int i = 0; i < im.options.threads; ++i) {
    im.threads.emplace_back([&im] { im.io.run(); });
  }
}

void Server::stop_accepting() {
  auto& im = *impl_;
  if (im.threads.empty()) return;
  std::promise<void> closed;
  auto done = closed.get_future();
  asio::post(im.io, [&im, &closed] {
    beast::error_code ec;
    im.acceptor.close(ec);
    closed.set_value();
  });
  done.wait();
}

int Server::open_connections() const { return impl_->open.load(); }

void Server::stop() {
  auto& im = *impl_;
  if (im.threads.empty()) return;
  asio::post(im.io, [&im] {
    beast::error_code ec;
    im.acceptor.close(ec);
  });
  im.work.reset();
  im.io.stop();
  for (auto& t : im.threads) t.join();
  im.threads.clear();
}

uint16_t Server::port() const { return impl_->bound_port; }

}  // namespace goober::server::http
