// The status vocabulary, conditional GETs, and CORS (037.Http.md; design/machine/011.Types.md §3).

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/encoding.h"
#include "goober/host/host.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace fs = std::filesystem;

namespace {

struct Reply {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};

Reply fetch(uint16_t port, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}) {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "localhost");
  for (const auto& [k, v] : headers) req.set(k, v);
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  Reply r{int(res.result_int()), res.body(), {}};
  for (const auto& f : res) r.headers[base::to_lower(std::string(f.name_string()))] = std::string(f.value());
  return r;
}

const char* kWat = R"wat((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_header_set" (func $header (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22n\22:1}")
  (data (i32.const 48) "location")
  (data (i32.const 64) "https://evil.example/")
  (data (i32.const 96) "/thanks")
  (func (export "goober_route_down") (result i32)
    (drop (call $respond (i32.const 503) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 7))) (i32.const 0))
  (func (export "goober_route_away") (result i32)
    (drop (call $header (i32.const 48) (i32.const 8) (i32.const 64) (i32.const 21)))
    (drop (call $respond (i32.const 303) (i32.const 0) (i32.const 16) (i32.const 0) (i32.const 0))) (i32.const 0))
  (func (export "goober_route_home") (result i32)
    (drop (call $header (i32.const 48) (i32.const 8) (i32.const 96) (i32.const 7)))
    (drop (call $respond (i32.const 303) (i32.const 0) (i32.const 16) (i32.const 0) (i32.const 0))) (i32.const 0))
  (func (export "goober_route_data") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 7))) (i32.const 0))
  (func (export "goober_route_nothing") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 7))) (i32.const 0))
))wat";

}  // namespace

TEST(HttpContract, StatusesEtagsAndCors) {
  auto root = fs::path(GOOBER_TEST_TMP) / "http_contract";
  fs::remove_all(root);
  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1}})",
                                  root, &err);
  ASSERT_TRUE(cfg) << err;
  host::Host h(config::local_mode(cfg));
  uint16_t port = h.start();
  ASSERT_TRUE(h.boxes().upload(R"({"box":"web","domains":["*"],"routes":{"http":{
      "/down":{"get":{"name":"down","public":true}},
      "/away":{"post":{"name":"away","public":true}},
      "/home":{"post":{"name":"home","public":true}},
      "/data":{"get":{"name":"data","public":true,"cors":["https://app.example.com"],"request-headers":["x-trace"]}},
      "/nothing":{"post":{"name":"nothing","public":true,"response":"empty","cors":["*"]}}}}})",
                               kWat, &err))
      << err;

  // 5xx is the host's: the box's body never reaches the caller.
  auto down = fetch(port, bhttp::verb::get, "/down", {{"accept", "application/json"}});
  EXPECT_EQ(down.status, 500);
  EXPECT_EQ(down.body.find(R"("n":1)"), std::string::npos);
  // A box may redirect within its site, never to another one.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/away").status, 500);
  auto home = fetch(port, bhttp::verb::post, "/home");
  EXPECT_EQ(home.status, 303);
  EXPECT_EQ(home.headers["location"], "/thanks");
  // An "empty" response is a 204 with no body, whatever the box sent.
  auto nothing = fetch(port, bhttp::verb::post, "/nothing");
  EXPECT_EQ(nothing.status, 204);
  EXPECT_TRUE(nothing.body.empty());

  // ETags, and 304 for a caller who has the bytes.
  auto data = fetch(port, bhttp::verb::get, "/data");
  ASSERT_EQ(data.status, 200);
  std::string etag = data.headers["etag"];
  EXPECT_EQ(etag.size(), 22u);
  auto again = fetch(port, bhttp::verb::get, "/data", {{"if-none-match", etag}});
  EXPECT_EQ(again.status, 304);
  EXPECT_TRUE(again.body.empty());

  // CORS: the listed origin may read; others and preflights are answered by the host.
  auto cors = fetch(port, bhttp::verb::get, "/data", {{"origin", "https://app.example.com"}});
  EXPECT_EQ(cors.headers["access-control-allow-origin"], "https://app.example.com");
  EXPECT_TRUE(cors.headers["access-control-allow-credentials"].empty());  // never with credentials
  auto stranger = fetch(port, bhttp::verb::get, "/data", {{"origin", "https://evil.example"}});
  EXPECT_TRUE(stranger.headers["access-control-allow-origin"].empty());
  auto pre = fetch(port, bhttp::verb::options, "/data", {{"origin", "https://app.example.com"}});
  EXPECT_EQ(pre.status, 204);
  EXPECT_EQ(pre.headers["access-control-allow-methods"], "GET, HEAD");
  EXPECT_NE(pre.headers["access-control-allow-headers"].find("x-trace"), std::string::npos);
  EXPECT_EQ(fetch(port, bhttp::verb::options, "/data", {{"origin", "https://evil.example"}}).status, 403);
  EXPECT_EQ(fetch(port, bhttp::verb::options, "/nothing", {{"origin", "https://anyone.example"}})
                .headers["access-control-allow-origin"],
            "*");
}
