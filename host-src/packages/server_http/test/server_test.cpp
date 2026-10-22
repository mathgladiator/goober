#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <thread>

#include "goober/server_http/server.h"

using namespace goober::server::http;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;

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
