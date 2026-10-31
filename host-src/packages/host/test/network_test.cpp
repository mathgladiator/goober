// External services and the outbox (design/machine/015.Network.md), against a stand-in
// service run by the test.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <mutex>
#include <thread>

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
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 2},
        "limits": {"outbox-daily-per-service": 3}})",
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
    ASSERT_TRUE(host->keys().set("system", "", "svc_key", "sk_test_secret", 1, &err, {"127.0.0.1"})) << err;
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
  // A key goes only to the hosts it names: set for another host, the call fails before it leaves.
  std::string err;
  ASSERT_TRUE(host->keys().set("system", "", "svc_key", "sk_test_secret", 2, &err, {"api.example.com"})) << err;
  ASSERT_EQ(post(port, "/ask", &body), 200);
  EXPECT_NE(body.find("may not be sent to 127.0.0.1"), std::string::npos) << body;
  EXPECT_EQ(stub.seen().size(), 1u);
  EXPECT_FALSE(host->keys().set("system", "", "svc_key", "x", 2, &err, {"not a host"}));
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

// Security review 4, S4-5: outside local mode a service's address must be public, whatever its
// name says, and the address checked is the one curl connects to.
TEST(SendHttp, ServicesReachOnlyPublicAddresses) {
  auto scratch = fs::path(GOOBER_TEST_TMP) / "send-http";
  for (const char* url : {"https://127.0.0.1:8443/admin", "https://localhost/x", "https://[::1]:8443/",
                          "https://10.0.0.5/", "https://169.254.169.254/latest/meta-data/"}) {
    auto r = host::send_http("get", url, {}, "", "", false, scratch, 2000);
    EXPECT_EQ(r.status, 0) << url;
    EXPECT_NE(r.error.find("private or reserved"), std::string::npos) << url << ": " << r.error;
  }
  // Nothing about the request is left behind.
  EXPECT_TRUE(base::list_files(scratch).empty());
}

// 035 § results: an effect whose outcome the box asked to hear. The host delivers it after the
// commit, as long as the method allows (a model can take a minute), then calls the box's
// result route with the outcome as a request of its own.
TEST_F(NetworkTest, ResultRoutesHearHowADeliveryWent) {
  const char* wat = R"wat((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_log" (func $log (param i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "model")
  (data (i32.const 24) "chat")
  (data (i32.const 32) "{\22body\22:{\22prompt\22:\22reply to Ann\22}}")
  (func (export "goober_route_think") (result i32) (local $n i32)
    (local.set $n (call $call (i32.const 16) (i32.const 5) (i32.const 24) (i32.const 4) (i32.const 32) (i32.const 34)))
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 4096) (local.get $n)))
    (i32.const 0))
  (func (export "goober_route_on_chat") (result i32) (local $n i32)
    (local.set $n (call $read (i32.const 8192) (call $size)))
    (drop (call $log (i32.const 1) (i32.const 8192) (local.get $n)))
    (i32.const 0))
))wat";
  std::string err;
  std::string manifest = R"({"box":"thinker","domains":["*"],
      "external":{"model":{"base":"http://127.0.0.1:)" + std::to_string(stub.port()) + R"(",
        "methods":{"chat":{"verb":"post","path":"/v1/chat/completions","result-route":"on_chat",
                           "delivery":{"attempts":2,"backoff-ms":100,"timeout-ms":60000}}}}},
      "routes":{"http":{"/think":{"post":{"name":"think","public":true}}},
                "result":{"on_chat":{"timeout-ms":2000}}}})";
  ASSERT_TRUE(host->boxes().upload(manifest, wat, &err)) << err;
  std::string body;
  ASSERT_EQ(post(port, "/think", &body), 200) << body;
  EXPECT_NE(body.find(R"("accepted":true)"), std::string::npos) << body;
  std::string key = std::string(json::parse(body, &err)->at("key").as_string());
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 1);
  // The result route heard the outcome: the key the call answered, and the service's reply.
  auto log = base::read_file(root / "storage/logs/boxes/thinker.log");
  ASSERT_TRUE(log);
  // (The log line holds the request as escaped JSON, so look for the parts.)
  EXPECT_NE(log->find("result:on_chat"), std::string::npos) << *log;
  EXPECT_NE(log->find(key), std::string::npos) << *log;
  EXPECT_NE(log->find("outcome"), std::string::npos) << *log;
  EXPECT_NE(log->find("done"), std::string::npos) << *log;
  EXPECT_NE(log->find("widget"), std::string::npos) << *log;

  // A method's result-route must name a result route, on an outbox method.
  EXPECT_FALSE(host->boxes().upload(R"({"box":"bad","domains":["*"],"external":{"m":{"base":"https://x.example",
      "methods":{"c":{"verb":"post","result-route":"nope"}}}}})", wat, &err));
  EXPECT_NE(err.find("no routes.result.nope"), std::string::npos) << err;
}

// The host's daily cap per tenant and service (limits.outbox-daily-per-service, 3 here): a box
// that loops learns in the call, and nothing past the cap is queued.
TEST_F(NetworkTest, OutboxCallsHaveADailyCapPerService) {
  std::string body;
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(post(port, "/send", &body), 200) << body;
    EXPECT_NE(body.find(R"("accepted":true)"), std::string::npos) << i << ": " << body;
  }
  ASSERT_EQ(post(port, "/send", &body), 200) << body;
  EXPECT_NE(body.find("limits.outbox-daily-per-service"), std::string::npos) << body;
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 3);
}

// goober-host vault key --create and the host can both make the vault key at once; they agree on
// one value, and an existing key is never replaced.
TEST(KeyStore, ASystemKeyIsMadeOnceEvenByTwoAtOnce) {
  auto storage = fs::path(GOOBER_TEST_TMP) / "ensure-key";
  fs::remove_all(storage);
  for (int round = 0; round < 20; ++round) {
    fs::remove_all(storage);
    std::string a, b;
    bool made_a = false, made_b = false;
    std::thread t1([&] {
      host::KeyStore keys(storage);
      std::string err;
      a = keys.ensure_random_system_key("vault_key", 1, &made_a, &err).value_or("");
    });
    std::thread t2([&] {
      host::KeyStore keys(storage);
      std::string err;
      b = keys.ensure_random_system_key("vault_key", 1, &made_b, &err).value_or("");
    });
    t1.join();
    t2.join();
    ASSERT_EQ(a.size(), 64u);
    EXPECT_EQ(a, b);
    EXPECT_NE(made_a, made_b);
  }
  host::KeyStore keys(storage);
  auto before = keys.resolve("keys/system/vault_key", "");
  bool made = true;
  std::string err;
  EXPECT_EQ(keys.ensure_random_system_key("vault_key", 2, &made, &err), before);
  EXPECT_FALSE(made);
  EXPECT_TRUE(base::list_files(storage / "keys/system").size() == 1);
}
