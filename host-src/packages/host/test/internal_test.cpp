// Internal routes (039; design/machine/016.Internal.md): boxes calling boxes.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace fs = std::filesystem;

namespace {

int fetch(uint16_t port, bhttp::verb verb, const std::string& target, const std::string& bearer, std::string* body) {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "localhost");
  if (!bearer.empty()) req.set(bhttp::field::authorization, "Bearer " + bearer);
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  if (body) *body = res.body();
  return int(res.result_int());
}

// billing: create_invoice echoes its request document; ping answers a fixed JSON.
const char* kBilling = R"wat((module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 32) "{\22pong\22:true}")
  (func (export "goober_route_create_invoice") (result i32)
    (local $n i32)
    (local.set $n (call $read (i32.const 1024) (call $size)))
    (drop (call $respond (i32.const 201) (i32.const 0) (i32.const 16) (i32.const 1024) (local.get $n)))
    (i32.const 0))
  (func (export "goober_route_ping") (result i32)
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 32) (i32.const 13)))
    (i32.const 0))
))wat";

// jobs: each route calls billing and answers with what came back.
const char* kJobs = R"wat((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "billing")
  (data (i32.const 32) "create_invoice")
  (data (i32.const 48) "ping")
  (data (i32.const 64) "{\22path\22:{\22job_id\22:\2242\22},\22body\22:{\22amount\22:100}}")
  (data (i32.const 128) "{}")
  (func $answer (param $n i32)
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 4096) (local.get $n))))
  (func (export "goober_route_run") (result i32)
    (call $answer (call $call (i32.const 16) (i32.const 7) (i32.const 32) (i32.const 14) (i32.const 64) (i32.const 46)))
    (i32.const 0))
  (func (export "goober_route_run_ping") (result i32)
    (call $answer (call $call (i32.const 16) (i32.const 7) (i32.const 48) (i32.const 4) (i32.const 128) (i32.const 2)))
    (i32.const 0))
))wat";

const char* kBillingManifest = R"({"box":"billing","domains":["*"],"types":"billing.openapi.json",
  "routes":{"internal":{
    "/jobs/{job_id : int64}/invoices":{"post":{"name":"create_invoice","callers":["jobs"],
                                               "requirements":[["owner"]],"body":"json:InvoiceRequest"}},
    "/ping":{"get":{"name":"ping","callers":["jobs"]}}}}})";
const char* kBillingTypes = R"({"openapi":"3.1.0","components":{"schemas":{"InvoiceRequest":{"type":"object",
  "required":["amount"],"additionalProperties":false,"properties":{"amount":{"type":"integer","format":"int32","minimum":1}}}}}})";
const char* kJobsManifest = R"({"box":"jobs","domains":["*"],
  "external":{"billing":{"box":"billing","methods":{"create_invoice":{"budget":{"calls":1}},"ping":{}}}},
  "routes":{"http":{"/run":{"post":{"name":"run","requirements":[],"agents":true}},
                    "/run-ping":{"post":{"name":"run_ping","public":true}}}}})";

}  // namespace

class InternalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "internal";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  // An agent token for a person holding `role` (or nothing).
  std::string token_for(const std::string& email, const std::string& role) {
    auto& id = host->identity();
    std::string err;
    int64_t now = base::now_unix_ms();
    if (!role.empty()) id.grant("playground", email, role, "test", now, &err);
    else id.grant("playground", email, "placeholder", "test", now, &err);
    int64_t user = 0;
    auto realm = id.describe("playground", now);  // kept: the loop below borrows from it
    for (const auto& u : realm.at("users").as_array()) {
      if (u.at("email").as_string() == email) user = std::stoll(std::string(u.at("user_id").as_string()));
    }
    auto agent = id.register_client("playground", "agent", {}, now, &err);
    identity::OAuthError e;
    auto d = id.start_device("playground", agent->client_id, now, &e);
    id.decide_device("playground", d->user_code, user, true, now, &err);
    return id.poll_device("playground", agent->client_id, d->device_code, now, 86400000, &e)->access_token;
  }
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(InternalTest, BoxesCallBoxesAsTheSamePerson) {
  std::string err, body;
  // Both sides must agree, so the caller can't come first.
  EXPECT_FALSE(host->boxes().upload(kJobsManifest, kJobs, &err));
  EXPECT_NE(err.find("isn't deployed"), std::string::npos) << err;
  ASSERT_TRUE(host->boxes().upload(kBillingManifest, kBilling, kBillingTypes, &err)) << err;
  ASSERT_TRUE(host->boxes().upload(kJobsManifest, kJobs, &err)) << err;

  // The owner's call goes through, carrying the person and the caller.
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/run", token_for("boss@example.com", "owner"), &body), 200) << body;
  std::string e;
  auto r = json::parse(body, &e)->as_object();
  EXPECT_EQ(r.at("status"), 201) << body;
  auto doc = r.at("json").as_object();
  EXPECT_EQ(doc.at("path"), "/jobs/42/invoices");
  EXPECT_EQ(doc.at("params").at("job_id"), "42");
  EXPECT_EQ(doc.at("body"), R"({"amount":100})");
  EXPECT_EQ(doc.at("caller").at("box"), "jobs");
  EXPECT_EQ(doc.at("caller").at("route"), "run");
  EXPECT_EQ(doc.at("user").at("email"), "boss@example.com");

  // Authority never grows: someone who isn't an owner gets nowhere through jobs.
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/run", token_for("guest@example.com", ""), &body), 200);
  EXPECT_NE(body.find(R"("status":403)"), std::string::npos) << body;

  // Routes without requirements serve whoever the caller serves; internal routes aren't reachable from outside.
  ASSERT_EQ(fetch(port, bhttp::verb::post, "/run-ping", "", &body), 200);
  EXPECT_NE(body.find(R"("pong":true)"), std::string::npos) << body;
  EXPECT_NE(fetch(port, bhttp::verb::get, "/ping", "", &body), 200);

  // The callee can't drop a caller that depends on it.
  std::string dropped = kBillingManifest;
  dropped.replace(dropped.find(R"(["jobs"],
                                               "requirements")"), 8, R"(["x"])");
  EXPECT_FALSE(host->boxes().upload(dropped, kBilling, kBillingTypes, &err));
  EXPECT_NE(err.find("callers don't list jobs"), std::string::npos) << err;

  // And no chain of calls may come back around.
  std::string loop = R"({"box":"billing","domains":["*"],"types":"billing.openapi.json",
    "external":{"j":{"box":"jobs","methods":{"anything":{}}}},
    "routes":{"internal":{"/ping":{"get":{"name":"ping","callers":["jobs"]}},
      "/jobs/{job_id : int64}/invoices":{"post":{"name":"create_invoice","callers":["jobs"],"body":"json:InvoiceRequest"}}}}})";
  EXPECT_FALSE(host->boxes().upload(loop, kBilling, kBillingTypes, &err));
}
