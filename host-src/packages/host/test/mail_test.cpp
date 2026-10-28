// Mail through Amazon SES, and mail that boxes send through the outbox
// (design/machine/018.Mail.md), against the stand-in AWS from the aws package's tests.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "../../aws/test/fake_aws.h"
#include "../src/keys.h"
#include "../src/mailer.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace fs = std::filesystem;

namespace {

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

// join: one welcome mail. bad: a mail with no subject, which the host refuses in the call.
const char* kWat = R"wat((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "mail")
  (data (i32.const 24) "send")
  (data (i32.const 32) "{\22to\22:\22Person@Example.org\22,\22subject\22:\22Welcome\22,\22text\22:\22Thanks for joining.\22,\22reply_to\22:\22owner@example.com\22}")
  (data (i32.const 160) "{\22to\22:\22person@example.org\22,\22text\22:\22no subject\22}")
  (data (i32.const 256) "{\22to\22:\22person@example.org\22,\22template\22:\22order-ready\22,\22vars\22:{\22name\22:\22<Ann>\22,\22email\22:\22evil@x.com\22}}")
  (data (i32.const 512) "{\22to\22:\22person@example.org\22,\22template\22:\22sign-in-new\22,\22vars\22:{\22code\22:\22123456\22}}")
  (func $answer (param $n i32)
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 4096) (local.get $n))))
  (func (export "goober_route_join") (result i32)
    (call $answer (call $call (i32.const 16) (i32.const 4) (i32.const 24) (i32.const 4) (i32.const 32) (i32.const 107)))
    (i32.const 0))
  (func (export "goober_route_tmpl") (result i32)
    (call $answer (call $call (i32.const 16) (i32.const 4) (i32.const 24) (i32.const 4) (i32.const 256) (i32.const 97)))
    (i32.const 0))
  (func (export "goober_route_forge") (result i32)
    (call $answer (call $call (i32.const 16) (i32.const 4) (i32.const 24) (i32.const 4) (i32.const 512) (i32.const 77)))
    (i32.const 0))
  (func (export "goober_route_bad") (result i32)
    (call $answer (call $call (i32.const 16) (i32.const 4) (i32.const 24) (i32.const 4) (i32.const 160) (i32.const 47)))
    (i32.const 0))
))wat";

const aws::Credentials kCreds{"AKIDTEST", "test-secret", ""};

class MailTest : public ::testing::Test {
 protected:
  void start(const std::string& extra) {
    root = fs::path(GOOBER_TEST_TMP) / "mail";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 2},
        "identity": {"mailer": "ses", "ses": {"from": "no-reply@example.com", "endpoint": ")" + fake.endpoint() + R"("}},
        "mail": {"per-address-daily": 1})" + extra + "}",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(cfg);
    port = host->start();
    std::string manifest = R"({"box":"welcome","domains":["*"],
      "external":{"mail":{"mail":{"from-name":"Beef Share"}}},
      "routes":{"http":{"/join":{"post":{"name":"join","public":true}},"/bad":{"post":{"name":"bad","public":true}},
        "/tmpl":{"post":{"name":"tmpl","public":true}},"/forge":{"post":{"name":"forge","public":true}}}}})";
    ASSERT_TRUE(host->boxes().upload(manifest, kWat, &err)) << err;
    ASSERT_TRUE(host->keys().set("system", "", "aws_access_key_id", kCreds.access_key_id, 1, &err)) << err;
    ASSERT_TRUE(host->keys().set("system", "", "aws_secret_access_key", kCreds.secret_access_key, 1, &err)) << err;
  }
  void TearDown() override { host.reset(); }

  aws::testing::FakeAws fake{kCreds};
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

}  // namespace

TEST_F(MailTest, BoxMailLeavesThroughSesAfterTheCommit) {
  start("");
  std::string body;
  ASSERT_EQ(post(port, "/join", &body), 200) << body;
  EXPECT_NE(body.find(R"("accepted":true)"), std::string::npos) << body;
  EXPECT_TRUE(fake.mails().empty());  // nothing before delivery
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 1);
  auto mails = fake.mails();
  ASSERT_EQ(mails.size(), 1u);
  EXPECT_NE(mails[0].find("From: \"Beef Share\" <no-reply@example.com>\r\n"), std::string::npos) << mails[0];
  EXPECT_NE(mails[0].find("To: person@example.org\r\n"), std::string::npos) << mails[0];
  EXPECT_NE(mails[0].find("Reply-To: owner@example.com\r\n"), std::string::npos) << mails[0];
  EXPECT_NE(mails[0].find("Subject: Welcome\r\n"), std::string::npos) << mails[0];
  EXPECT_NE(mails[0].find("\r\n\r\nThanks for joining."), std::string::npos) << mails[0];
  auto log = base::read_file(root / "storage/logs/mail.log");
  ASSERT_TRUE(log);
  EXPECT_NE(log->find(R"("via":"ses")"), std::string::npos) << *log;
  EXPECT_NE(log->find(R"("message_id":"fake-1")"), std::string::npos) << *log;
  EXPECT_EQ(fake.refused_signatures(), 0);

  // One mail a day to an address (mail.per-address-daily 1): the second is refused for good.
  ASSERT_EQ(post(port, "/join"), 200);
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 0);
  EXPECT_EQ(fake.mails().size(), 1u);
  auto dead = host->outbox_list("dead");
  ASSERT_EQ(dead.size(), 1u);
  EXPECT_NE(std::string(dead[0].at("last_error").as_string()).find("per-address-daily"), std::string::npos);
}

// Mail templates (022): a box sends one of the tenant's own templates by name, wrapped in the
// layout, to the address it names; never one of the host's own, like a sign-in code.
TEST_F(MailTest, BoxesSendTheirTenantsTemplatesButNeverTheHostsOwn) {
  start("");
  std::string err, body;
  ASSERT_TRUE(host->mail_templates().put("playground", "order-ready",
                                         {"Your order is ready, {{name}}", "Pick it up at {{site}}, {{name}}.", ""}, &err))
      << err;
  ASSERT_EQ(post(port, "/tmpl", &body), 200) << body;
  EXPECT_NE(body.find(R"("accepted":true)"), std::string::npos) << body;
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 1);
  auto mails = fake.mails();
  ASSERT_EQ(mails.size(), 1u);
  EXPECT_NE(mails[0].find("To: person@example.org\r\n"), std::string::npos) << mails[0];
  EXPECT_NE(mails[0].find("Subject: Your order is ready, <Ann>\r\n"), std::string::npos) << mails[0];
  EXPECT_NE(mails[0].find("Pick it up at localhost, <Ann>."), std::string::npos) << mails[0];
  EXPECT_NE(mails[0].find("Sent by localhost to person@example.org."), std::string::npos) << mails[0];
  EXPECT_NE(mails[0].find("Content-Type: text/html"), std::string::npos) << mails[0];

  ASSERT_EQ(post(port, "/forge", &body), 200);
  EXPECT_NE(body.find("one of the host's own mails"), std::string::npos) << body;
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 0);
}

TEST_F(MailTest, SesHiccupsAreRetriedAndRefusalsAreNot) {
  start("");
  ASSERT_EQ(post(port, "/join"), 200);
  int64_t now = base::now_unix_ms();
  fake.fail_next(1, 503);
  EXPECT_EQ(host->outbox_tick(now), 0);
  EXPECT_TRUE(host->outbox_list("dead").empty());
  EXPECT_EQ(host->outbox_tick(now + 60000), 1);  // after its backoff
  EXPECT_EQ(fake.mails().size(), 1u);
}

TEST_F(MailTest, BadMailIsRefusedInTheCall) {
  start("");
  std::string body;
  ASSERT_EQ(post(port, "/bad", &body), 200);
  EXPECT_NE(body.find("subject is 1 to 200"), std::string::npos) << body;
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 0);
  EXPECT_TRUE(fake.mails().empty());
}

TEST_F(MailTest, ASandboxSendsNothing) {
  start(R"(, "sandbox": true)");
  ASSERT_EQ(post(port, "/join"), 200);
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 0);
  EXPECT_TRUE(fake.mails().empty());
  EXPECT_EQ(host->outbox_list("pending").size(), 1u);  // waiting, visibly
  EXPECT_EQ(fake.requests(), 0);
}

TEST(MailQueue, SignInMailGoesThroughSes) {
  aws::testing::FakeAws fake(kCreds);
  auto root = fs::path(GOOBER_TEST_TMP) / "mail-queue";
  fs::remove_all(root);
  std::string err;
  auto cfg = config::parse_config(R"({"storage": "storage", "identity": {"mailer": "ses",
      "ses": {"from": "no-reply@example.com", "endpoint": ")" + fake.endpoint() + R"("}}})", root, &err);
  ASSERT_TRUE(cfg) << err;
  bool have_creds = true;
  int dev_mails = 0;
  host::MailQueue q([&] { return cfg; }, [&] { return have_creds ? std::optional(kCreds) : std::nullopt; },
                    [&](const identity::Mail&) { ++dev_mails; });
  q.send(identity::Mail{"person@example.org", "Your code", "123456"});
  q.drain();
  ASSERT_EQ(fake.mails().size(), 1u);
  EXPECT_NE(fake.mails()[0].find("From: \"goober\" <no-reply@example.com>"), std::string::npos);
  have_creds = false;
  auto outcome = q.deliver(identity::Mail{"person@example.org", "x", "y"});
  EXPECT_FALSE(outcome.sent);
  EXPECT_NE(outcome.error.find("no AWS credentials"), std::string::npos);
  EXPECT_EQ(dev_mails, 0);
}
