// External services and the outbox (design/machine/015.Network.md), against a stand-in
// service run by the test.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <mutex>

#include "../src/keys.h"
#include "../src/network.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/server_http/server.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

struct Seen {
  std::string method, target, body, auth, idem;
};

// The stand-in service: records each request and answers with the next queued status.
class Stub {
 public:
  Stub() {
    server::http::ServerOptions o;
    o.port = 0;
    o.threads = 1;
    server_ = std::make_unique<server::http::Server>(o, [this](server::http::Request req, server::http::Responder respond) {
      server::http::Response res;
      {
        std::lock_guard lock(mu_);
        seen_.push_back(Seen{req.method, req.target, req.body, req.header("authorization"), req.header("idempotency-key")});
        res.status = statuses_.empty() ? 200 : statuses_.front();
        if (!statuses_.empty()) statuses_.erase(statuses_.begin());
      }
      res.headers.emplace_back("content-type", "application/json");
      res.body = R"({"ok":true,"item":"widget"})";
      respond(std::move(res));
    });
    server_->start();
  }
  uint16_t port() const { return server_->port(); }
  void answer(std::vector<int> statuses) {
    std::lock_guard lock(mu_);
    statuses_ = std::move(statuses);
  }
  std::vector<Seen> seen() {
    std::lock_guard lock(mu_);
    return seen_;
  }

 private:
  std::mutex mu_;
  std::vector<Seen> seen_;
  std::vector<int> statuses_;
  std::unique_ptr<server::http::Server> server_;
};

int post(uint16_t port, const std::string& target, std::string* body = nullptr) {
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
  if (body) *body = res.body();
  return int(res.result_int());
}

// ask: a safe call, answered with what came back. send: a note and an outbox element.
// send_fail: the same, then a 500, so neither lands.
const char* kWat = R"wat((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "svc")
  (data (i32.const 20) "lookup")
  (data (i32.const 32) "send")
  (data (i32.const 48) "{\22path\22:{\22id\22:\22a b/c\22},\22query\22:{\22q\22:\22x\22}}")
  (data (i32.const 112) "{\22body\22:{\22text\22:\22hi\22,\22tenant\22:\22forged\22}}")
  (data (i32.const 176) "db")
  (data (i32.const 192) "INSERT INTO notes (text) VALUES ('sent')")
  (data (i32.const 240) "[]")
  (func $answer (param $status i32) (param $n i32)
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (local.get $status) (i32.const 0) (i32.const 16) (i32.const 4096) (local.get $n))))
  (func $send (param $status i32)
    (drop (call $sql (i32.const 176) (i32.const 2) (i32.const 192) (i32.const 40) (i32.const 240) (i32.const 2)))
    (call $answer (local.get $status)
      (call $call (i32.const 16) (i32.const 3) (i32.const 32) (i32.const 4) (i32.const 112) (i32.const 40))))
  (func (export "goober_route_ask") (result i32)
    (call $answer (i32.const 200)
      (call $call (i32.const 16) (i32.const 3) (i32.const 20) (i32.const 6) (i32.const 48) (i32.const 41)))
    (i32.const 0))
  (func (export "goober_route_send") (result i32) (call $send (i32.const 200)) (i32.const 0))
  (func (export "goober_route_send_fail") (result i32) (call $send (i32.const 500)) (i32.const 0))
))wat";

}  // namespace

class NetworkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "network";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    ASSERT_TRUE(host->schemas().put(R"({"schema":"notify1","tables":{"notes":{"columns":{"text":{"type":"string"}}}}})", &err))
        << err;
    std::string manifest = R"({"box":"notify","domains":["*"],
      "data":{"sqlite3":{"db":{"schema":"notify1","database":"notify","pattern":"sql","tables":{"notes":{"read":true,"insert":true}}}}},
      "external":{"svc":{"base":"http://127.0.0.1:)" + std::to_string(stub.port()) + R"(","credentials":"keys/system/svc_key",
        "methods":{
          "lookup":{"verb":"get","path":"/items/{id}"},
          "send":{"verb":"post","path":"/messages","idempotency":"header/Idempotency-Key",
                  "require":{"body.tenant":"{!tenant}"},"delivery":{"attempts":3,"backoff-ms":100}}}}},
      "routes":{"http":{"/ask":{"post":{"name":"ask","public":true}},"/send":{"post":{"name":"send","public":true}},
                        "/send-fail":{"post":{"name":"send_fail","public":true}}}}})";
    ASSERT_TRUE(host->boxes().upload(manifest, kWat, &err)) << err;
    ASSERT_TRUE(host->keys().set("system", "", "svc_key", "sk_test_secret", 1, &err)) << err;
  }
  void TearDown() override { host.reset(); }
  int64_t notes() {
    std::string err;
    auto r = host->databases().operate("playground", "notify", "SELECT count(*) FROM notes", nullptr, 10, false, &err);
    return r ? r->at("rows").at(0).at(0).as_int64() : -1;
  }

  Stub stub;
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(NetworkTest, SafeCallsGoNowWithTheKeyTheBoxNeverSees) {
  std::string body;
  ASSERT_EQ(post(port, "/ask", &body), 200) << body;
  EXPECT_NE(body.find(R"("status":200)"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("item":"widget")"), std::string::npos) << body;
  auto seen = stub.seen();
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].method, "get");
  EXPECT_EQ(seen[0].target, "/items/a%20b%2Fc?q=x");  // a value can't add a path segment
  EXPECT_EQ(seen[0].auth, "Bearer sk_test_secret");
  // Without the key, the call fails in the box's hands, and nothing goes out.
  host->keys().remove("system", "", "svc_key");
  ASSERT_EQ(post(port, "/ask", &body), 200);
  EXPECT_NE(body.find("isn't set"), std::string::npos) << body;
  EXPECT_EQ(stub.seen().size(), 1u);
  // The egress log says what went where, and never the key.
  auto log = base::read_file(root / "storage/logs/egress.log");
  ASSERT_TRUE(log);
  EXPECT_NE(log->find(R"("service":"svc")"), std::string::npos);
  EXPECT_EQ(log->find("sk_test_secret"), std::string::npos);
}

TEST_F(NetworkTest, EffectfulCallsWaitForTheCommitAndRetryWithOneKey) {
  std::string body;
  ASSERT_EQ(post(port, "/send", &body), 200) << body;
  EXPECT_NE(body.find(R"("accepted":true)"), std::string::npos) << body;
  EXPECT_TRUE(stub.seen().empty());  // nothing goes out before delivery
  EXPECT_EQ(notes(), 1);

  // A failed request leaves neither its note nor its call behind.
  EXPECT_EQ(post(port, "/send-fail"), 500);
  EXPECT_EQ(notes(), 1);

  int64_t now = base::now_unix_ms();
  stub.answer({503});
  EXPECT_EQ(host->outbox_tick(now), 0);           // the service hiccups
  EXPECT_EQ(host->outbox_tick(now + 50), 0);      // still backing off
  EXPECT_EQ(host->outbox_tick(now + 1000), 1);    // and then it lands
  auto seen = stub.seen();
  ASSERT_EQ(seen.size(), 2u);  // two attempts for the one committed element, none for the failed request
  EXPECT_EQ(seen[0].idem, seen[1].idem);  // the same key on every attempt
  EXPECT_FALSE(seen[0].idem.empty());
  EXPECT_EQ(seen[1].target, "/messages");
  EXPECT_EQ(seen[1].body, R"({"text":"hi","tenant":"playground"})");  // require wins over what the box wrote
  EXPECT_EQ(host->outbox_tick(now + 5000), 0);    // delivered once

  // A permanent refusal is dead-lettered; an operator can try it again.
  ASSERT_EQ(post(port, "/send"), 200);
  stub.answer({400});
  EXPECT_EQ(host->outbox_tick(now + 6000), 0);
  auto dead = host->outbox_list("dead");
  ASSERT_EQ(dead.size(), 1u);
  EXPECT_EQ(dead[0].at("last_status"), 400);
  int64_t id = dead[0].at("id").as_int64();
  EXPECT_TRUE(host->outbox_set("playground", "notify", id, true));
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 1);
  EXPECT_TRUE(host->outbox_list("dead").empty());
}

TEST(BuildCall, PathValuesCanNeitherAddNorWalkSegments) {
  manifest::ExternalService svc;
  svc.base = "https://api.example.com/v1";
  manifest::ExternalMethod m;
  m.verb = "get";
  m.path = "/customers/{id}";
  host::CallContext ctx;
  std::string err;
  for (const char* bad : {"..", ".", ""}) {
    EXPECT_FALSE(host::build_call(svc, m, bj::object{{"path", bj::object{{"id", bad}}}}, ctx, &err)) << bad;
  }
  auto ok = host::build_call(svc, m, bj::object{{"path", bj::object{{"id", "../admin"}}}}, ctx, &err);
  ASSERT_TRUE(ok) << err;
  EXPECT_EQ(ok->url, "https://api.example.com/v1/customers/..%2Fadmin");
}
