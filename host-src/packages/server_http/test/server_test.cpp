#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <atomic>
#include <future>
#include <thread>

#include "goober/server_http/server.h"

using namespace goober::server::http;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
using tcp = asio::ip::tcp;

namespace {
bhttp::response<bhttp::string_body> fetch(uint16_t port, bhttp::verb verb, const std::string& target,
                                          const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "localhost");
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response_parser<bhttp::string_body> parser;
  parser.skip(verb == bhttp::verb::head);
  bhttp::read(stream, buf, parser);
  return parser.release();
}
}  // namespace

TEST(Server, AnswersFromAnotherThread) {
  Server server({"127.0.0.1", 0, 1, 1024}, [](Request req, Responder respond) {
    std::thread([req = std::move(req), respond = std::move(respond)] {
      Response res;
      res.headers.emplace_back("content-type", "text/plain");
      res.body = req.method + " " + req.path + " " + req.query + " " + req.body + " " + req.header("host");
      res.head_only = req.method == "head";
      respond(std::move(res));
    }).detach();
  });
  server.start();
  auto r = fetch(server.port(), bhttp::verb::post, "/a/b?x=1", "payload");
  EXPECT_EQ(r.result_int(), 200);
  EXPECT_EQ(r.body(), "post /a/b x=1 payload localhost");
  auto h = fetch(server.port(), bhttp::verb::head, "/a");
  EXPECT_EQ(h.result_int(), 200);
  EXPECT_EQ(h[bhttp::field::content_length], "19");
  server.stop();
}

TEST(Server, EnforcesBodyLimit) {
  Server server({"127.0.0.1", 0, 1, 16}, [](Request, Responder respond) { respond(Response{}); });
  server.start();
  auto r = fetch(server.port(), bhttp::verb::post, "/", std::string(100, 'x'));
  EXPECT_EQ(r.result_int(), 413);
  server.stop();
}

// Open streams (design/machine/036.Bus.md § 4): headers, then writes in order, until either side
// closes, and a reader that falls too far behind is let go.
TEST(Server, StreamsStayOpenAndCloseFromEitherSide) {
  std::shared_ptr<Stream> held;
  std::promise<void> opened;
  std::atomic<bool> closed_seen{false};
  Server server({"127.0.0.1", 0, 2, 1024}, [&](Request, Responder respond) {
    Response res;
    res.headers.emplace_back("content-type", "text/event-stream");
    res.stream = [&](std::shared_ptr<Stream> s) {
      s->on_close([&] { closed_seen = true; });
      held = s;
      s->send("data: one\n\n");
      opened.set_value();
    };
    respond(std::move(res));
  });
  server.start();
  asio::io_context io;
  tcp::socket sock(io);
  sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.port()));
  std::string req = "GET /live HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n";
  asio::write(sock, asio::buffer(req));
  opened.get_future().wait();
  ASSERT_TRUE(held->send("data: two\n\n"));
  std::string got;
  char buf[1024];
  while (got.find("data: two") == std::string::npos) {
    size_t n = sock.read_some(asio::buffer(buf));
    got.append(buf, n);
  }
  EXPECT_TRUE(got.starts_with("HTTP/1.1 200 OK\r\n")) << got;
  EXPECT_NE(got.find("content-type: text/event-stream\r\n"), std::string::npos);
  EXPECT_NE(got.find("connection: close\r\n"), std::string::npos);
  EXPECT_EQ(got.find("content-length"), std::string::npos);
  EXPECT_LT(got.find("data: one"), got.find("data: two"));
  // The client goes away: the stream closes and says so.
  sock.close();
  for (int i = 0; i < 200 && !closed_seen; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(closed_seen);
  EXPECT_TRUE(held->closed());
  EXPECT_FALSE(held->send("data: three\n\n"));
  server.stop();
}

TEST(Server, AStreamTooFarBehindIsLetGo) {
  std::shared_ptr<Stream> held;
  std::promise<void> opened;
  Server server({"127.0.0.1", 0, 1, 1024}, [&](Request, Responder respond) {
    Response res;
    res.stream_max_behind = 64 * 1024;
    res.stream = [&](std::shared_ptr<Stream> s) {
      held = s;
      opened.set_value();
    };
    respond(std::move(res));
  });
  server.start();
  asio::io_context io;
  tcp::socket sock(io);
  sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.port()));
  asio::write(sock, asio::buffer(std::string("GET / HTTP/1.1\r\nHost: x\r\n\r\n")));
  opened.get_future().wait();
  // The client reads nothing: the kernel's buffers fill, then the queue, then it's let go.
  bool sent = true;
  for (int i = 0; i < 100000 && sent; ++i) sent = held->send(std::string(8192, 'x'));
  EXPECT_FALSE(sent);
  EXPECT_TRUE(held->closed());
  server.stop();
}

// A client that sends Expect: 100-continue is told to go on at once.
TEST(Server, AnswersExpectContinue) {
  ServerOptions o{"127.0.0.1", 0, 1, 1024};
  o.body_limit_for = [](std::string_view, std::string_view) -> uint64_t { return 1024; };
  Server server(o, [](Request req, Responder respond) {
    Response r;
    r.body = "got " + std::to_string(req.body.size());
    respond(std::move(r));
  });
  server.start();
  asio::io_context io;
  tcp::socket sock(io);
  sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.port()));
  std::string head = "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n";
  asio::write(sock, asio::buffer(head));
  // Before any body is sent, the 100 arrives.
  asio::streambuf buf;
  asio::read_until(sock, buf, "\r\n\r\n");
  std::string got((std::istreambuf_iterator<char>(&buf)), {});
  EXPECT_TRUE(got.starts_with("HTTP/1.1 100 Continue")) << got;
  asio::write(sock, asio::buffer(std::string("hello")));
  beast::flat_buffer fb;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(sock, fb, res);
  EXPECT_EQ(res.result_int(), 200u);
  EXPECT_EQ(res.body(), "got 5");
}

namespace {
// A raw request on a fresh connection, and whatever comes back before the server closes it
// or `wait_ms` passes.
std::string raw(uint16_t port, const std::string& text, int wait_ms = 2000) {
  asio::io_context io;
  tcp::socket sock(io);
  sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  asio::write(sock, asio::buffer(text));
  sock.non_blocking(true);
  std::string got;
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  char buf[4096];
  while (std::chrono::steady_clock::now() < until) {
    boost::system::error_code ec;
    size_t n = sock.read_some(asio::buffer(buf), ec);
    if (ec == asio::error::would_block) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (ec) break;
    got.append(buf, n);
  }
  return got;
}
}  // namespace

// Framing nobody agrees on is refused: a transfer coding that isn't chunked last, two Hosts.
TEST(Server, RefusesAmbiguousFraming) {
  ServerOptions o{"127.0.0.1", 0, 1, 1024};
  o.body_limit_for = [](std::string_view, std::string_view) -> uint64_t { return 0; };
  Server server(o, [](Request, Responder respond) { respond(Response{}); });
  server.start();
  EXPECT_TRUE(raw(server.port(), "POST /x HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: xchunked\r\n\r\n0\r\n\r\n")
                  .starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(raw(server.port(), "GET /x HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n").starts_with("HTTP/1.1 400"));
  auto plain = raw(server.port(), "GET /x HTTP/1.1\r\nHost: a\r\n\r\n");
  EXPECT_TRUE(plain.starts_with("HTTP/1.1 200")) << plain;
}

// A body must keep coming; one client may send only so many at once.
TEST(Server, SlowBodiesAreBoundedPerClientAndByProgress) {
  ServerOptions o{"127.0.0.1", 0, 2, 1 << 20};
  o.body_limit_for = [](std::string_view, std::string_view) -> uint64_t { return 0; };
  o.client_key_for = [](std::string_view, std::string_view xff) { return std::string(xff); };
  o.max_bodies_per_client = 2;
  o.body_window_bytes = 100;
  o.body_window_seconds = 1;
  Server server(o, [](Request req, Responder respond) {
    Response r;
    r.body = std::to_string(req.body.size());
    respond(std::move(r));
  });
  server.start();
  // Two slow bodies from one client: the third is refused at once; another client isn't.
  asio::io_context io;
  std::vector<std::unique_ptr<tcp::socket>> held;
  for (int i = 0; i < 2; ++i) {
    held.push_back(std::make_unique<tcp::socket>(io));
    held.back()->connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.port()));
    asio::write(*held.back(), asio::buffer(std::string("POST /u HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 1.1.1.1\r\nContent-Length: 1000\r\n\r\n")));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(raw(server.port(), "POST /u HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 1.1.1.1\r\nContent-Length: 5\r\n\r\nhello")
                  .starts_with("HTTP/1.1 429"));
  EXPECT_TRUE(raw(server.port(), "POST /u HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 2.2.2.2\r\nContent-Length: 5\r\n\r\nhello")
                  .starts_with("HTTP/1.1 200"));
  // The held ones send nothing more: past the window they're closed, and their places come back.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_TRUE(raw(server.port(), "POST /u HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 1.1.1.1\r\nContent-Length: 5\r\n\r\nhello")
                  .starts_with("HTTP/1.1 200"));
  // A slow but steady body (100 bytes every half second, over 2.5 s) is read to the end.
  {
    tcp::socket sock(io);
    sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.port()));
    asio::write(sock, asio::buffer(std::string("POST /u HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 3.3.3.3\r\nContent-Length: 500\r\n\r\n")));
    for (int i = 0; i < 5; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      asio::write(sock, asio::buffer(std::string(100, 'x')));
    }
    beast::flat_buffer fb;
    bhttp::response<bhttp::string_body> res;
    bhttp::read(sock, fb, res);
    EXPECT_EQ(res.body(), "500");
  }
}

// A request whose client closes the connection before its answer is marked gone (the MCP queue
// drops such a call rather than run it, design/machine/047.Controls.md § 13); one whose client
// waits, or sends a next request on the same connection, is not.
TEST(Server, MarksARequestWhoseClientLeftBeforeItsAnswer) {
  std::mutex mu;
  std::vector<std::pair<std::shared_ptr<const std::atomic<bool>>, Responder>> pending;
  Server server({"127.0.0.1", 0, 1, 1024}, [&](Request req, Responder respond) {
    std::lock_guard lock(mu);
    pending.emplace_back(req.peer_gone, std::move(respond));
  });
  server.start();
  auto send = [&](tcp::socket& s, const std::string& path) {
    std::string text = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    asio::write(s, asio::buffer(text));
  };
  auto wait_for = [&](size_t n) {
    for (int i = 0; i < 400; ++i) {
      {
        std::lock_guard lock(mu);
        if (pending.size() >= n) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  };
  asio::io_context io;
  tcp::socket leaves(io), stays(io), pipelines(io);
  tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), server.port());
  leaves.connect(ep);
  send(leaves, "/a");
  ASSERT_TRUE(wait_for(1));
  stays.connect(ep);
  send(stays, "/b");
  ASSERT_TRUE(wait_for(2));
  pipelines.connect(ep);
  send(pipelines, "/c");
  ASSERT_TRUE(wait_for(3));
  send(pipelines, "/d");  // a next request while /c waits
  leaves.close();
  bool marked = false;
  for (int i = 0; i < 400 && !marked; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::lock_guard lock(mu);
    marked = pending[0].first && pending[0].first->load();
  }
  EXPECT_TRUE(marked);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    std::lock_guard lock(mu);
    ASSERT_TRUE(pending[1].first && pending[2].first);
    EXPECT_FALSE(pending[1].first->load());
    EXPECT_FALSE(pending[2].first->load());
    for (auto& [g, respond] : pending) respond(Response{});  // answering the one that left is harmless
  }
  // The pipelined request is still served once /c is answered.
  ASSERT_TRUE(wait_for(4));
  {
    std::lock_guard lock(mu);
    pending[3].second(Response{});
  }
  beast::flat_buffer buf;
  for (int i = 0; i < 2; ++i) {
    bhttp::response<bhttp::string_body> res;
    bhttp::read(pipelines, buf, res);
    EXPECT_EQ(res.result_int(), 200);
  }
  server.stop();
}
