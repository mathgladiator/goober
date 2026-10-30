// The bus and subscriptions (design/machine/036.Bus.md) end to end: a write that commits
// publishes, a subscriber hears it over SSE, a rolled-back write doesn't, subscribers with the
// same signature share one evaluation, and an evaluation changes nothing.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <regex>
#include <thread>

#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;
using tcp = asio::ip::tcp;

namespace {

const char* kWat = R"wat((module
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "w")
  (data (i32.const 16) "INSERT INTO items (name) VALUES ('x')")
  (data (i32.const 128) "SELECT count(*) AS n FROM items")
  (data (i32.const 192) "application/json")
  (data (i32.const 224) "ok")
  (func $answer (param $len i32)
    (drop (call $result (i32.const 1024) (local.get $len)))
    (drop (call $respond (i32.const 200) (i32.const 192) (i32.const 16) (i32.const 1024) (local.get $len))))
  (func (export "goober_route_add") (result i32)
    (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 37) (i32.const 0) (i32.const 0)))
    (drop (call $respond (i32.const 201) (i32.const 192) (i32.const 16) (i32.const 224) (i32.const 2)))
    (i32.const 0))
  (func (export "goober_route_add_fail") (result i32)
    (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 37) (i32.const 0) (i32.const 0)))
    unreachable)
  (func (export "goober_route_live") (result i32)
    (call $answer (call $sql (i32.const 0) (i32.const 1) (i32.const 128) (i32.const 31) (i32.const 0) (i32.const 0)))
    (i32.const 0))
  (func (export "goober_route_sneaky") (result i32)
    (call $answer (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 37) (i32.const 0) (i32.const 0)))
    (i32.const 0))
))wat";

const char* kSchema = R"({"schema":"items1","tables":{"items":{"columns":{"name":{"type":"string","max-length":40}}}}})";

const char* kManifest = R"({"box":"shop","domains":["*"],
  "data":{"sqlite3":{"w":{"schema":"items1","database":"items","pattern":"sql",
                          "tables":{"items":{"read":true,"insert":true}},"publish":"items"}}},
  "routes":{
    "http":{"/items":{"post":{"name":"add","public":true}},"/fail":{"post":{"name":"add_fail","public":true}}},
    "sse":{"/items/live":{"subscribe":{"name":"live","public":true,"response":"json","wake":["items"],
                                       "coalesce-ms":50,"refresh-ms":60000}},
           "/sneaky":{"subscribe":{"name":"sneaky","public":true,"wake":["items"]}}}}})";

int post(uint16_t port, const std::string& target) {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(bhttp::verb::post, target, 11);
  req.set(bhttp::field::host, "localhost");
  req.set(bhttp::field::origin, "http://localhost");
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  return int(res.result_int());
}

// One open subscription, read as text.
class Subscriber {
 public:
  Subscriber(uint16_t port, const std::string& target, const std::string& last_event_id = "") : sock_(io_) {
    sock_.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    std::string req = "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n" +
                      (last_event_id.empty() ? "" : "Last-Event-ID: " + last_event_id + "\r\n") + "\r\n";
    asio::write(sock_, asio::buffer(req));
    sock_.non_blocking(true);
  }
  // Waits until the text read so far holds `needle` n times, or the time runs out.
  bool wait_for(const std::string& needle, int times, int ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
      pump();
      if (count(needle) >= times) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }
  int count(const std::string& needle) {
    int n = 0;
    for (size_t at = text_.find(needle); at != std::string::npos; at = text_.find(needle, at + 1)) ++n;
    return n;
  }
  const std::string& text() {
    pump();
    return text_;
  }

 private:
  void pump() {
    char buf[4096];
    boost::system::error_code ec;
    while (true) {
      size_t n = sock_.read_some(asio::buffer(buf), ec);
      if (ec || n == 0) break;
      text_.append(buf, n);
    }
  }
  asio::io_context io_;
  tcp::socket sock_;
  std::string text_;
};

}  // namespace

class BusTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "bus";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 2, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    ASSERT_TRUE(host->schemas().put(kSchema, &err)) << err;
    auto schema = host->schemas().get("items1");
    ASSERT_TRUE(host->databases().open("playground", "items", &*schema, &err)) << err;
    ASSERT_TRUE(host->boxes().upload(kManifest, kWat, &err)) << err;
  }
  void TearDown() override { host.reset(); }
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(BusTest, AWriteThatCommitsWakesSubscribersAndOneThatFailsDoesnt) {
  Subscriber a(port, "/items/live");
  ASSERT_TRUE(a.wait_for("event: update", 1, 3000)) << a.text();
  EXPECT_NE(a.text().find("content-type: text/event-stream"), std::string::npos);
  EXPECT_NE(a.text().find("data: {\"columns\":[\"n\"],\"rows\":[[0]]"), std::string::npos) << a.text();
  // A second subscriber with the same signature shares the group, and gets the cached answer.
  Subscriber b(port, "/items/live");
  ASSERT_TRUE(b.wait_for("rows\":[[0]]", 1, 3000)) << b.text();
  auto status = host->bus_status("playground");
  ASSERT_EQ(status.at("subscriptions").as_array().size(), 1u) << json::dump(status);  // /items/live, one group
  EXPECT_EQ(status.at("subscriptions").at(0).at("subscribers").as_int64(), 2);
  int64_t evaluations = status.at("subscriptions").at(0).at("evaluations").as_int64();

  EXPECT_EQ(post(port, "/items"), 201);
  ASSERT_TRUE(a.wait_for("event: update", 2, 3000)) << a.text();
  ASSERT_TRUE(b.wait_for("rows\":[[1]]", 1, 3000)) << b.text();
  EXPECT_NE(a.text().find("rows\":[[1]]"), std::string::npos);
  status = host->bus_status("playground");
  EXPECT_EQ(status.at("subscriptions").at(0).at("evaluations").as_int64(), evaluations + 1);  // shared
  EXPECT_EQ(status.at("topics").at(0).at("topic"), "items");
  // Event ids are the bus's sequences, moving forward.
  std::string t = a.text();
  std::vector<int64_t> ids;
  std::regex id_rx("id: ([0-9]+)");
  for (auto it = std::sregex_iterator(t.begin(), t.end(), id_rx); it != std::sregex_iterator(); ++it) {
    ids.push_back(std::stoll((*it)[1]));
  }
  ASSERT_GE(ids.size(), 2u);
  EXPECT_GT(ids.back(), ids.front());

  // A write that rolls back publishes nothing.
  EXPECT_EQ(post(port, "/fail"), 500);
  EXPECT_FALSE(a.wait_for("event: update", 3, 500)) << a.text();

  // Reconnecting with the last id seen: nothing to send until the next change.
  Subscriber c(port, "/items/live", std::to_string(ids.back()));
  EXPECT_FALSE(c.wait_for("event: update", 1, 400)) << c.text();
  EXPECT_EQ(post(port, "/items"), 201);
  EXPECT_TRUE(c.wait_for("rows\":[[2]]", 1, 3000)) << c.text();
}

TEST_F(BusTest, AnEvaluationChangesNothingAndAPlainGetAnswersOnce) {
  Subscriber s(port, "/sneaky");
  ASSERT_TRUE(s.wait_for("event: update", 1, 3000)) << s.text();
  EXPECT_NE(s.text().find("a subscription changes nothing"), std::string::npos) << s.text();
  // Nothing was inserted: a plain GET of the live route says 0.
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(bhttp::verb::get, "/items/live", 11);
  req.set(bhttp::field::host, "localhost");
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  EXPECT_EQ(res.result_int(), 200u);
  EXPECT_NE(res.body().find("rows\":[[0]]"), std::string::npos) << res.body();
}

TEST(Manifest, SubscriptionsAndPublishAreChecked) {
  std::string err;
  EXPECT_TRUE(manifest::parse_manifest(kManifest, &err)) << err;
  for (const char* bad : {
           R"({"box":"b","domains":["*"],"routes":{"sse":{"/x":{"get":{"name":"x","public":true}}}}})",
           R"({"box":"b","domains":["*"],"routes":{"sse":{"/x":{"subscribe":{"name":"x","public":true}}}}})",
           R"({"box":"b","domains":["*"],"routes":{"sse":{"/x":{"subscribe":{"name":"x","public":true,"wake":["a b"]}}}}})",
           R"({"box":"b","domains":["*"],"routes":{"sse":{"/x":{"subscribe":{"name":"x","public":true,"wake":["{id}"]}}}}})",
           R"({"box":"b","domains":["*"],"routes":{"sse":{"/x":{"subscribe":{"name":"x","public":true,"wake":["mine/{!user_id}"]}}}}})",
           R"({"box":"b","domains":["*"],"routes":{"sse":{"/x":{"subscribe":{"name":"x","public":true,"wake":["a"],"coalesce-ms":1}}}}})",
           R"({"box":"b","domains":["*"],"routes":{"sse":{"/x":{"subscribe":{"name":"x","public":true,"wake":["a"],"body":"text"}}}}})"}) {
    EXPECT_FALSE(manifest::parse_manifest(bad, &err)) << bad;
  }
}
