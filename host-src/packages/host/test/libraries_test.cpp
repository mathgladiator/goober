// Host libraries from a box (design/machine/831.Libraries.md): a manifest imports time@1 and
// ics@1, and the box calls them through goober_lib; the host checks imports at upload.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

std::string post(uint16_t port, const std::string& target) {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(bhttp::verb::post, target, 11);
  req.set(bhttp::field::host, "localhost");
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  return res.body();
}

std::string wat_text(const std::string& s) {
  std::string out;
  for (char c : s) {
    out += c == '"' ? std::string("\\22") : c == '\\' ? std::string("\\5c") : c == '\n' ? std::string("\\0a") : std::string(1, c);
  }
  return out;
}

// A box whose route calls one library function with a fixed argument and answers the result.
std::string box(const std::string& lib, const std::string& fn, const std::string& arg, const std::string& route = "go") {
  return R"((module
  (import "goober" "goober_lib" (func $lib (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) ")" + lib + R"(")
  (data (i32.const 48) ")" + fn + R"(")
  (data (i32.const 64) ")" + wat_text(arg) + R"(")
  (func (export "goober_route_)" + route + R"(") (result i32) (local $n i32)
    (local.set $n (call $lib (i32.const 32) (i32.const )" + std::to_string(lib.size()) + R"() (i32.const 48) (i32.const )" +
         std::to_string(fn.size()) + R"() (i32.const 64) (i32.const )" + std::to_string(arg.size()) + R"()))
    (drop (call $result (i32.const 8192) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 8192) (local.get $n)))
    (i32.const 0)))
)";
}

class LibrariesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "libraries";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }
  bj::object json_of(const std::string& s) {
    std::string err;
    auto v = json::parse(s, &err);
    EXPECT_TRUE(v && v->is_object()) << s;
    return v ? v->as_object() : bj::object{};
  }
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

}  // namespace

TEST_F(LibrariesTest, ABoxCallsTimeAndIcsThroughGooberLib) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"clock","domains":["*"],"libraries":{"t":"time@1"},
      "routes":{"http":{"/clock":{"post":{"name":"go","public":true}}}}})",
                                   box("t", "format", R"({"ms":1793923200000,"zone":"America/Chicago"})"), &err))
      << err;
  auto f = json_of(post(port, "/clock"));
  EXPECT_EQ(f["local"], "2026-11-05T18:00:00");
  EXPECT_EQ(f["weekday"], "thursday");
  // The access log line says a library was called, and which tzdata answered.
  auto log = base::read_file(root / "storage/logs/access.log");
  ASSERT_TRUE(log);
  EXPECT_NE(log->find(R"("libraries":1)"), std::string::npos);
  EXPECT_NE(log->find(R"("tzdata":")"), std::string::npos);

  ASSERT_TRUE(host->boxes().upload(R"({"box":"cal","domains":["*"],"libraries":{"ics":"ics@1"},
      "routes":{"http":{"/cal":{"post":{"name":"cal_go","public":true}}}}})",
                                   box("ics", "invite", R"({"event":{"uid":"m7@localhost","summary":"Dinner",
          "start":{"local":"2026-11-05T18:00:00","zone":"America/Chicago"}},
          "organizer":{"email":"host@localhost"},"attendees":[{"email":"ada@example.org"}]})", "cal_go"),
                                   &err))
      << err;
  auto inv = json_of(post(port, "/cal"));
  ASSERT_TRUE(inv.contains("ics")) << json::dump(inv);
  EXPECT_NE(std::string(inv["ics"].as_string()).find("METHOD:REQUEST"), std::string::npos);
  EXPECT_EQ(inv["event"].at("attendees").at(0).at("partstat"), "NEEDS-ACTION");
}

TEST_F(LibrariesTest, ImportsAreCheckedAndUndeclaredLibrariesAnswerAnError) {
  std::string err;
  EXPECT_FALSE(host->boxes().upload(R"({"box":"x","domains":["*"],"libraries":{"t":"time@9"},
      "routes":{"http":{"/x":{"post":{"name":"x_go","public":true}}}}})",
                                    box("t", "now", "{}", "x_go"), &err));
  EXPECT_NE(err.find("no time@9 (it has time@1, ics@1)"), std::string::npos) << err;
  // Declared as "t", called as "time": refused in the call, never a trap.
  ASSERT_TRUE(host->boxes().upload(R"({"box":"y","domains":["*"],"libraries":{"t":"time@1"},
      "routes":{"http":{"/y":{"post":{"name":"y_go","public":true}}}}})",
                                   box("time", "now", "{}", "y_go"), &err))
      << err;
  EXPECT_EQ(json_of(post(port, "/y"))["code"], "unknown_library");
  ASSERT_TRUE(host->boxes().upload(R"({"box":"z","domains":["*"],
      "routes":{"http":{"/z":{"post":{"name":"z_go","public":true}}}}})",
                                   box("time", "now", "{}", "z_go"), &err))
      << err;
  EXPECT_EQ(json_of(post(port, "/z"))["code"], "unknown_library");
}
