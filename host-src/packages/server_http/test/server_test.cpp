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
