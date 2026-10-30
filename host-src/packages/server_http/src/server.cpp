#include "goober/server_http/server.h"

#include <fcntl.h>

#include <atomic>
#include <deque>
#include <map>
#include <future>
#include <mutex>
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

// Request bodies being read, per client (ServerOptions::max_bodies_per_client).
struct BodyCounts {
  std::mutex mu;
  std::map<std::string, int> by_client;
};

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(tcp::socket socket, const ServerOptions& options, const Handler& handler, std::atomic<int>& open,
          BodyCounts& bodies)
      : stream_(std::move(socket)), options_(options), handler_(handler), open_(open), bodies_(bodies) {
    ++open_;
  }
  ~Session() {
    release_body();
    --open_;
  }

  void run() {
    asio::dispatch(stream_.get_executor(), [self = shared_from_this()] { self->read(); });
  }

 private:
  void read() {
    parser_.emplace();
    parser_->body_limit(options_.body_max_bytes);
    parser_->header_limit(64 * 1024);
    stream_.expires_after(std::chrono::seconds(options_.idle_timeout_seconds));
    if (!options_.body_limit_for) {
      bhttp::async_read(stream_, buffer_, *parser_,
                        [self = shared_from_this()](beast::error_code ec, size_t) { self->on_read(ec); });
      return;
    }
    // Headers first, so the body's limit can depend on who is sending it. Beast checks a
    // declared length against the limit as the header completes, so the limit is lifted until
    // the header is in and then set for real.
    parser_->body_limit(boost::none);
    bhttp::async_read_header(stream_, buffer_, *parser_, [self = shared_from_this()](beast::error_code ec, size_t) {
      if (ec) return self->on_read(ec);
      const auto& h = self->parser_->get();
      uint64_t limit = 0;
      try {
        limit = self->options_.body_limit_for(std::string_view(h.target().data(), h.target().size()),
                                              std::string_view(h[bhttp::field::authorization].data(),
                                                               h[bhttp::field::authorization].size()));
      } catch (...) {
        limit = 0;
      }
      limit = std::max<uint64_t>(limit, self->options_.body_max_bytes);
      // A declared length past the limit is refused now, before a byte of the body is read;
      // a chunked body is held to the limit as it arrives.
      if (auto length = self->parser_->content_length(); length && *length > limit) {
        return self->on_read(bhttp::error::body_limit);
      }
      self->parser_->body_limit(limit);
      // A transfer coding other than chunked last is no framing anyone agrees on (RFC 9112
      // § 6.1), and two Hosts are two answers to which site: both refused.
      if (h.count(bhttp::field::host) > 1) return self->reply_error(400, "Bad Request");
      if (auto te = h[bhttp::field::transfer_encoding]; !te.empty() && !self->parser_->chunked()) {
        return self->reply_error(400, "Bad Request");
      }
      if (!self->parser_->is_done()) {
        // One client's bodies in flight are counted, so slow ones can't hold every connection.
        beast::error_code pe;
        auto peer = self->stream_.socket().remote_endpoint(pe);
        std::string peer_text = pe ? std::string() : peer.address().to_string();
        std::string key = peer_text;
        try {
          if (self->options_.client_key_for) {
            auto xff = h["x-forwarded-for"];
            key = self->options_.client_key_for(peer_text, std::string_view(xff.data(), xff.size()));
          }
        } catch (...) {
        }
        {
          std::lock_guard lock(self->bodies_.mu);
          int& n = self->bodies_.by_client[key];
          if (n >= self->options_.max_bodies_per_client) {
            if (n == 0) self->bodies_.by_client.erase(key);
            self->keep_alive_ = false;
            return self->reply_error(429, "Too Many Requests");
          }
          ++n;
          self->counted_ = key;
          self->counting_ = true;
        }
      }
      auto read_body = [self] {
        self->body_seen_ = 0;
        self->window_mark_ = 0;
        self->stream_.expires_after(std::chrono::seconds(self->options_.body_window_seconds));
        self->read_body_some();
      };
      // A client that asked first (curl does above 1 MiB, and so do agents built on it) is told
      // to go on now, rather than waiting a second before sending anyway.
      if (!self->parser_->is_done() && beast::iequals(h[bhttp::field::expect], "100-continue")) {
        static const std::string kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
        return asio::async_write(self->stream_, asio::buffer(kContinue), [self, read_body](beast::error_code ec2, size_t) {
          if (ec2) return self->on_read(ec2);
          read_body();
        });
      }
      read_body();
    });
  }

  // The body, piece by piece: each window that brings enough bytes earns another window.
  void read_body_some() {
    if (parser_->is_done()) {  // no body, or all of it already in the buffer
      return asio::post(stream_.get_executor(), [self = shared_from_this()] { self->on_read({}); });
    }
    bhttp::async_read_some(stream_, buffer_, *parser_, [self = shared_from_this()](beast::error_code ec, size_t n) {
      if (ec) return self->on_read(ec);
      if (self->parser_->is_done()) return self->on_read({});
      self->body_seen_ += n;
      if (self->body_seen_ - self->window_mark_ >= self->options_.body_window_bytes) {
        self->window_mark_ = self->body_seen_;
        self->stream_.expires_after(std::chrono::seconds(self->options_.body_window_seconds));
      }
      self->read_body_some();
    });
  }

  void release_body() {
    if (!counting_) return;
    counting_ = false;
    std::lock_guard lock(bodies_.mu);
    auto it = bodies_.by_client.find(counted_);
    if (it != bodies_.by_client.end() && --it->second <= 0) bodies_.by_client.erase(it);
  }

  void on_read(beast::error_code ec) {
    release_body();
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
    if (res.stream && !res.head_only) return open_stream(std::move(res));
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

  // An open stream: its own queue of writes on the session's strand. It holds the session
  // until it closes; the session holds nothing of it.
  class Open : public Stream, public std::enable_shared_from_this<Open> {
   public:
    Open(std::shared_ptr<Session> s, size_t max_behind) : session_(std::move(s)), max_behind_(max_behind) {}
    using Stream::send;
    bool send(std::shared_ptr<const std::string> bytes) override {
      std::shared_ptr<Session> s;
      bool start = false, over = false;
      {
        std::lock_guard lock(mu_);
        if (closed_) return false;
        if (queued_ + bytes->size() > max_behind_) {
          // Falling behind: a reader this slow is let go rather than let it grow the queue.
          over = true;
        } else {
          queued_ += bytes->size();
          queue_.push_back(std::move(bytes));
          start = !writing_;
          writing_ = writing_ || start;
          s = session_;
        }
      }
      if (over) {
        close();
        return false;
      }
      if (start && s) asio::post(s->stream_.get_executor(), [self = shared_from_this()] { self->pump(); });
      return true;
    }
    void close() override {
      std::vector<std::function<void()>> callbacks;
      std::shared_ptr<Session> s;
      {
        std::lock_guard lock(mu_);
        if (closed_) return;
        closed_ = true;
        callbacks.swap(on_close_);
        s = std::move(session_);
      }
      if (s) {
        asio::post(s->stream_.get_executor(), [s] {
          beast::error_code ec;
          s->stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
          s->stream_.socket().close(ec);
        });
      }
      for (auto& f : callbacks) f();
    }
    bool closed() const override {
      std::lock_guard lock(mu_);
      return closed_;
    }
    void on_close(std::function<void()> f) override {
      {
        std::lock_guard lock(mu_);
        if (!closed_) {
          on_close_.push_back(std::move(f));
          return;
        }
      }
      f();
    }
    // Reads until the client goes away; anything it sends is ignored.
    void watch_peer() {
      std::shared_ptr<Session> s;
      {
        std::lock_guard lock(mu_);
        s = session_;
      }
      if (!s) return;
      s->stream_.socket().async_read_some(asio::buffer(sink_), [self = shared_from_this()](beast::error_code ec, size_t) {
        if (ec) return self->close();
        self->watch_peer();
      });
    }

   private:
    void pump() {
      std::shared_ptr<Session> s;
      {
        std::lock_guard lock(mu_);
        if (closed_ || queue_.empty()) {
          writing_ = false;
          return;
        }
        current_ = std::move(queue_.front());
        queue_.pop_front();
        s = session_;
      }
      if (!s) return;
      asio::async_write(s->stream_.socket(), asio::buffer(*current_), [self = shared_from_this()](beast::error_code ec, size_t n) {
        {
          std::lock_guard lock(self->mu_);
          self->queued_ -= std::min(self->queued_, n);
        }
        if (ec) return self->close();
        self->pump();
      });
    }

    mutable std::mutex mu_;
    std::shared_ptr<Session> session_;
    size_t max_behind_, queued_ = 0;
    std::deque<std::shared_ptr<const std::string>> queue_;
    std::shared_ptr<const std::string> current_;
    bool writing_ = false, closed_ = false;
    std::vector<std::function<void()>> on_close_;
    char sink_[512];
  };

  void open_stream(Response res) {
    keep_alive_ = false;
    unsigned code = unsigned(res.status);
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " +
                       std::string(bhttp::obsolete_reason(static_cast<bhttp::status>(res.status))) + "\r\n";
    for (const auto& [k, v] : res.headers) head += k + ": " + v + "\r\n";
    head += "server: goober\r\nconnection: close\r\n\r\n";
    stream_.expires_never();
    auto open = std::make_shared<Open>(shared_from_this(), res.stream_max_behind + head.size());
    open->send(std::move(head));
    open->watch_peer();
    try {
      res.stream(open);
    } catch (...) {
      open->close();
    }
  }

  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  std::optional<bhttp::request_parser<bhttp::string_body>> parser_;
  const ServerOptions& options_;
  const Handler& handler_;
  std::atomic<int>& open_;
  BodyCounts& bodies_;
  std::string counted_;
  bool counting_ = false;
  uint64_t body_seen_ = 0, window_mark_ = 0;
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
          // Never inherited by a child process (asio's accept doesn't set it).
          ::fcntl(socket.native_handle(), F_SETFD, FD_CLOEXEC);
          // No Nagle: a response written in two pieces (headers past Beast's buffer count, then
          // the body) would otherwise wait out the client's delayed ACK, about 40 ms.
          beast::error_code nd;
          socket.set_option(tcp::no_delay(true), nd);
          std::make_shared<Session>(std::move(socket), options, handler, open, bodies)->run();
        }
      }
      accept();
    });
  }

  ServerOptions options;
  std::atomic<int> open{0};
  BodyCounts bodies;
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
  ::fcntl(im.acceptor.native_handle(), F_SETFD, FD_CLOEXEC);
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
