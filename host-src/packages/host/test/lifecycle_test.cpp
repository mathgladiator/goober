// The host's health endpoint and graceful shutdown (PROGRESS.md Phase 3).

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <fstream>
#include <thread>

#include "goober/host/host.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace fs = std::filesystem;

namespace {
int get(uint16_t port, const std::string& target, std::string* body = nullptr, const std::string& host = "localhost") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  beast::error_code cec;
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), cec);
  if (cec) return -1;
  bhttp::request<bhttp::string_body> req(bhttp::verb::get, target, 11);
  req.set(bhttp::field::host, host);
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  beast::error_code ec;
  bhttp::read(stream, buf, res, ec);
  if (ec) return -1;
  if (body) *body = res.body();
  return int(res.result_int());
}

// A route that works for a few hundred milliseconds, then answers.
const char* kSlow = R"((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "text/plain")
  (func (export "goober_route_slow") (result i32)
    (local $i i32)
    (loop $l (local.set $i (i32.add (local.get $i) (i32.const 1))) (br_if $l (i32.lt_u (local.get $i) (i32.const 250000000))))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 10) (i32.const 0) (i32.const 0)))
    (i32.const 0))
))";
}  // namespace

TEST(Lifecycle, HealthAndGracefulShutdown) {
  auto root = fs::path(GOOBER_TEST_TMP) / "lifecycle";
  fs::remove_all(root);
  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1}})",
                                  root, &err);
  ASSERT_TRUE(cfg) << err;
  host::Host h(config::local_mode(cfg));
  uint16_t port = h.start();
  std::string body;
  EXPECT_EQ(get(port, "/goober/health", &body), 200);
  EXPECT_TRUE(body.starts_with(R"({"ok":true,"build":")") && body.ends_with("\"}")) << body;  // the commit
  // A proxy's health check names the upstream address, which is no configured domain.
  EXPECT_EQ(get(port, "/goober/health", &body, "10.1.2.3:8080"), 200);
  EXPECT_NE(get(port, "/", &body, "10.1.2.3:8080"), 200);
  ASSERT_TRUE(h.boxes().upload(R"({"box":"slow","domains":["*"],"behavior":{"timeout-ms":5000,"fuel":2000000000},
      "routes":{"http":{"/slow":{"get":{"name":"slow","public":true}}}}})",
                               kSlow, &err))
      << err;
  // A request in flight when the host is told to stop still gets its answer.
  int status = 0;
  auto started = std::chrono::steady_clock::now();
  std::string slow_body;
  std::thread caller([&] { status = get(port, "/slow", &slow_body); });
  // Time for the request to arrive before stop (20 ms was sometimes too little on a busy machine);
  // the slow route runs about 80 ms, under the host's 2e9 fuel cap.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  h.stop();
  auto stopped_after = std::chrono::steady_clock::now() - started;
  caller.join();
  EXPECT_EQ(status, 200) << slow_body;
  EXPECT_GT(stopped_after, std::chrono::milliseconds(50));  // stop() waited for the request
  EXPECT_EQ(get(port, "/goober/health"), -1);  // and then nothing is listening
}

// A corrupt shared store is never a crash (systemd would restart the host forever): the host
// serves, /goober/ready names what's off, and with consent unreadable no mail goes out.
TEST(Lifecycle, CorruptStoresAreSetAsideOrHeldNotFatal) {
  auto root = fs::path(GOOBER_TEST_TMP) / "lifecycle_corrupt";
  fs::remove_all(root);
  std::string junk(8192, 'x');
  for (const char* f : {"storage/mail/consent.db", "storage/web/web.db", "storage/source/source.db"}) {
    fs::create_directories((root / f).parent_path());
    std::ofstream(root / f) << junk;
  }
  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1}})",
                                  root, &err);
  ASSERT_TRUE(cfg) << err;
  host::Host h(config::local_mode(cfg));
  uint16_t port = h.start();
  std::string body;
  EXPECT_EQ(get(port, "/goober/health", &body), 200);
  auto [status, ready] = h.readiness(false);
  EXPECT_EQ(status, 503);
  auto off = boost::json::serialize(ready.at("checks").at("stores"));
  for (const char* s : {"consent", "web", "source"}) EXPECT_NE(off.find(s), std::string::npos) << off;
  // Set aside, not deleted: the bytes are kept for recovery; consent.db is left where it is.
  bool web_kept = false, source_kept = false;
  for (const auto& e : fs::directory_iterator(root / "storage/web")) web_kept |= e.path().filename().string().starts_with("web.db.unreadable-");
  for (const auto& e : fs::directory_iterator(root / "storage/source")) source_kept |= e.path().filename().string().starts_with("source.db.unreadable-");
  EXPECT_TRUE(web_kept);
  EXPECT_TRUE(source_kept);
  EXPECT_EQ(fs::file_size(root / "storage/mail/consent.db"), junk.size());
  // The preferences page refuses rather than lose a choice.
  EXPECT_EQ(get(port, "/goober/mail/playground/abc/def"), 503);
  h.stop();
}
