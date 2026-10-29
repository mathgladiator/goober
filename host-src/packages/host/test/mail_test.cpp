// Mail through Amazon SES, and mail that boxes send through the outbox
// (design/machine/018.Mail.md), against the stand-in AWS from the aws package's tests.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "../../aws/test/fake_aws.h"
#include "../src/keys.h"
#include "../src/mail_hash.h"
#include "../src/mailer.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/sqlite/sqlite.h"

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
        "domains": {"localhost": {"tenant": "playground"}, "kc.example": {"tenant": "playground"}},
        "threads": {"network": 1, "boxes": 2},
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

// A box that answers fan@example.org in a thread, from the tenant's own address.
void upload_outreach(host::Host& host, const std::string& in_reply_to = "abc@mail.example.org") {
  std::string send = R"({"to":"Fan@Example.org","subject":"Re: Brisket","text":"Thanks for writing.",)"
                     R"("in_reply_to":")" + in_reply_to + R"(","references":"<root@mail.example.org> abc@mail.example.org"})";
  std::string escaped;
  for (char c : send) escaped += c == '"' ? std::string("\\22") : std::string(1, c);
  std::string wat = R"((module
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "application/json")
  (data (i32.const 16) "mail")
  (data (i32.const 24) "send")
  (data (i32.const 64) ")" + escaped + R"(")
  (func (export "goober_route_reach") (result i32) (local $n i32)
    (local.set $n (call $call (i32.const 16) (i32.const 4) (i32.const 24) (i32.const 4) (i32.const 64) (i32.const )" +
                    std::to_string(send.size()) + R"()))
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 4096) (local.get $n)))
    (i32.const 0)))
)";
  std::string err;
  ASSERT_TRUE(host.boxes().upload(
      R"({"box":"outreach","domains":["*"],"external":{"mail":{"mail":{"from-name":"KC Meat Up","from":"hello@kc.example"}}},)"
      R"("routes":{"http":{"/reach":{"post":{"name":"reach","public":true}}}}})",
      wat, &err))
      << err;
}

// Reaching people who never signed up (018 § 3): mail comes from the tenant's own address,
// answers in the person's thread, and carries a one-click unsubscribe that ends every box's
// mail to them.
TEST_F(MailTest, OutreachThreadsAndUnsubscribesForGood) {
  start("");
  upload_outreach(*host);
  std::string body;
  ASSERT_EQ(post(port, "/reach", &body), 200) << body;
  EXPECT_NE(body.find(R"("accepted":true)"), std::string::npos) << body;
  ASSERT_EQ(host->outbox_tick(base::now_unix_ms()), 1);
  auto mails = fake.mails();
  ASSERT_EQ(mails.size(), 1u);
  const std::string& m = mails[0];
  EXPECT_NE(m.find("From: \"KC Meat Up\" <hello@kc.example>\r\n"), std::string::npos) << m;
  EXPECT_NE(m.find("In-Reply-To: <abc@mail.example.org>\r\n"), std::string::npos) << m;
  EXPECT_NE(m.find("References: <root@mail.example.org> <abc@mail.example.org>\r\n"), std::string::npos) << m;
  EXPECT_NE(m.find("List-Unsubscribe-Post: List-Unsubscribe=One-Click\r\n"), std::string::npos) << m;
  auto at = m.find("List-Unsubscribe: <https://kc.example/goober/unsubscribe/playground/");
  ASSERT_NE(at, std::string::npos) << m;
  EXPECT_NE(m.find("Unsubscribe: https://kc.example/goober/unsubscribe/"), std::string::npos) << m;  // the footer
  // logs/mail.log names the tenant and the From, which mail_activity reads.
  auto mail_log = base::read_file(root / "storage/logs/mail.log");
  ASSERT_TRUE(mail_log);
  EXPECT_NE(mail_log->find(R"("tenant":"playground")"), std::string::npos) << *mail_log;
  EXPECT_NE(mail_log->find(R"("from":"hello@kc.example")"), std::string::npos) << *mail_log;
  std::string url = m.substr(at + 19, m.find('>', at) - at - 19);
  std::string path = url.substr(std::string("https://kc.example").size());

  auto request = [&](bhttp::verb verb, const std::string& target) {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    bhttp::request<bhttp::string_body> req(verb, target, 11);
    req.set(bhttp::field::host, "kc.example");
    if (verb == bhttp::verb::post) {
      req.set(bhttp::field::content_type, "application/x-www-form-urlencoded");
      req.body() = "List-Unsubscribe=One-Click";
    }
    req.prepare_payload();
    bhttp::write(stream, req);
    beast::flat_buffer buf;
    bhttp::response<bhttp::string_body> res;
    bhttp::read(stream, buf, res);
    return std::pair<int, std::string>(int(res.result_int()), res.body());
  };
  // Opening the link (as a mail scanner does) asks; it doesn't unsubscribe.
  auto asked = request(bhttp::verb::get, path);
  EXPECT_EQ(asked.first, 200);
  EXPECT_NE(asked.second.find("Stop these emails?"), std::string::npos);
  EXPECT_FALSE(host->mail_suppressed("playground", "fan@example.org"));
  // A forged link does nothing.
  EXPECT_EQ(request(bhttp::verb::post, path.substr(0, path.size() - 1) + (path.back() == '0' ? "1" : "0")).first, 404);
  EXPECT_FALSE(host->mail_suppressed("playground", "fan@example.org"));
  // The one-click POST unsubscribes.
  auto done = request(bhttp::verb::post, path);
  EXPECT_EQ(done.first, 200);
  EXPECT_NE(done.second.find("unsubscribed"), std::string::npos);
  EXPECT_TRUE(host->mail_suppressed("playground", "FAN@example.org"));
  // Every box of the tenant is refused in the call from then on.
  ASSERT_EQ(post(port, "/reach", &body), 200);
  EXPECT_NE(body.find("unsubscribed"), std::string::npos) << body;
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 0);
  EXPECT_EQ(fake.mails().size(), 1u);
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

// A conversation isn't outreach (018 § 3): an answer to a message the person sent this tenant
// gets mail.reply-per-address-daily, while a made-up In-Reply-To stays under the outreach cap.
TEST_F(MailTest, RepliesInAThreadThePersonStartedGetMoreRoom) {
  start("");
  std::string body;
  // Outreach: the fixture's mail.per-address-daily is 1, so the second is refused.
  upload_outreach(*host, "never-sent@elsewhere.example");
  ASSERT_EQ(post(port, "/reach", &body), 200) << body;
  ASSERT_EQ(post(port, "/reach", &body), 200) << body;
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 1);
  EXPECT_EQ(fake.mails().size(), 1u);

  // The person wrote to the tenant (mail in noted their address and Message-ID): answers to it
  // go out past the outreach cap.
  {
    sqlite::Db inbound(root / "storage/mail/inbound.db");
    inbound.exec("CREATE TABLE IF NOT EXISTS received (tenant TEXT NOT NULL, sender TEXT NOT NULL, message TEXT NOT NULL, "
                 "at_ms INTEGER NOT NULL, PRIMARY KEY (tenant, sender, message))");
    sqlite::Stmt s(inbound, "INSERT INTO received (tenant, sender, message, at_ms) VALUES (?, ?, ?, ?)");
    s.bind(1, std::string("playground"))
        .bind(2, host::received_hash("fan@example.org"))
        .bind(3, host::received_hash("<abc@mail.example.org>"))
        .bind(4, base::now_unix_ms())
        .run();
  }
  upload_outreach(*host);
  ASSERT_EQ(post(port, "/reach", &body), 200) << body;
  ASSERT_EQ(post(port, "/reach", &body), 200) << body;
  host->outbox_tick(base::now_unix_ms());
  EXPECT_EQ(fake.mails().size(), 3u);
}
