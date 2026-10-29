// Mail in (038.Mail.md; design/machine/024.MailIn.md): the host as Stalwart's MTA hook. These
// tests post what Stalwart posts and check the answers it would turn into SMTP replies.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "goober/base/files.h"
#include "../src/mail_hash.h"
#include "goober/host/host.h"
#include "goober/sqlite/sqlite.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

const std::string kToken = "stalwart-test-token-0123456789abcdef";

struct Reply {
  int status = 0;
  std::string body;
};

Reply post(uint16_t port, const std::string& target, const std::string& body,
           std::vector<std::pair<std::string, std::string>> headers) {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(bhttp::verb::post, target, 11);
  req.set(bhttp::field::host, "127.0.0.1:8080");
  req.set(bhttp::field::content_type, "application/json");
  for (const auto& [k, v] : headers) req.set(k, v);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  return Reply{int(res.result_int()), res.body()};
}

// support_inbox, ticket_reply, and inbox_all keep what they get; refuse_inbox says no; later_inbox
// asks to be tried again; notify sends mail with a reply address and answers the call's result.
const char* kWat = R"wat((module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_sql" (func $sql (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_call" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))
  (import "goober" "goober_result_read" (func $result (param i32 i32) (result i32)))
  (memory (export "memory") 4)
  (data (i32.const 0) "w")
  (data (i32.const 16) "INSERT INTO got (path, body) VALUES (:path, :body)")
  (data (i32.const 128) "application/json")
  (data (i32.const 160) "{\22error\22:\22No thanks\22}")
  (data (i32.const 200) "mail")
  (data (i32.const 208) "send")
  (data (i32.const 224) "{\22to\22:\22cy@example.org\22,\22subject\22:\22About ticket 42\22,\22text\22:\22Reply to this.\22,\22reply\22:{\22route\22:\22ticket_reply\22,\22params\22:{\22ticket\22:\2242\22}}}")
  (func $store
    (local $n i32)
    (local.set $n (call $read (i32.const 8192) (call $size)))
    (drop (call $sql (i32.const 0) (i32.const 1) (i32.const 16) (i32.const 50) (i32.const 8192) (local.get $n))))
  (func (export "goober_route_support_inbox") (result i32) (call $store) (i32.const 0))
  (func (export "goober_route_ticket_reply") (result i32) (call $store) (i32.const 0))
  (func (export "goober_route_inbox_all") (result i32) (call $store) (i32.const 0))
  (func (export "goober_route_refuse_inbox") (result i32)
    (drop (call $respond (i32.const 400) (i32.const 128) (i32.const 16) (i32.const 160) (i32.const 21)))
    (i32.const 0))
  (func (export "goober_route_later_inbox") (result i32)
    (drop (call $respond (i32.const 503) (i32.const 128) (i32.const 16) (i32.const 160) (i32.const 0)))
    (i32.const 0))
  (func (export "goober_route_notify") (result i32)
    (local $n i32)
    (local.set $n (call $call (i32.const 200) (i32.const 4) (i32.const 208) (i32.const 4) (i32.const 224) (i32.const 133)))
    (drop (call $result (i32.const 4096) (local.get $n)))
    (drop (call $respond (i32.const 200) (i32.const 128) (i32.const 16) (i32.const 4096) (local.get $n)))
    (i32.const 0))
))wat";

const char* kSchema = R"({"schema":"got1","tables":{"got":{"columns":{
  "path":{"type":"string"},"body":{"type":"string"}}}}})";

const char* kDesk = R"({"box":"desk","domains":["kcmeatup.com"],
  "data":{"sqlite3":{"w":{"schema":"got1","database":"got","pattern":"sql","tables":{"got":{"read":true,"insert":true}}}}},
  "external":{"mail":{"mail":{"from-name":"KC Meat Up"}}},
  "routes":{
    "http":{"/notify":{"post":{"name":"notify","public":true}}},
    "mail":{
      "support@":{"receive":{"name":"support_inbox","public":true,"max-bytes":4096,"attachments":1}},
      "refuse@":{"receive":{"name":"refuse_inbox","public":true}},
      "later@kcmeatup.com":{"receive":{"name":"later_inbox","public":true}},
      "reply+{ticket : token}@":{"receive":{"name":"ticket_reply","public":true}}}}})";

const char* kInbox = R"({"box":"inbox","domains":["kcmeatup.com"],
  "data":{"sqlite3":{"w":{"schema":"got1","database":"got","pattern":"sql","tables":{"got":{"read":true,"insert":true}}}}},
  "routes":{"mail":{"*@":{"receive":{"name":"inbox_all","public":true,"automatic":true}}}}})";

// What Stalwart posts at a stage.
std::string hook(const std::string& stage, const std::vector<std::string>& to, const std::string& raw_headers = "",
                 const std::string& body = "", const std::string& from = "ann@example.org") {
  bj::array rcpts;
  for (const auto& t : to) rcpts.push_back(bj::object{{"address", t}});
  bj::object o{{"context", bj::object{{"stage", stage},
                                     {"client", bj::object{{"ip", "203.0.113.9"}, {"port", 40000}, {"ptr", nullptr},
                                                           {"helo", "mx.example.org"}, {"activeConnections", 1}}},
                                     {"server", bj::object{{"name", "Stalwart"}, {"port", 25}, {"ip", "127.0.0.1"}}},
                                     {"protocol", bj::object{{"version", 1}}}}},
               {"envelope", bj::object{{"from", bj::object{{"address", from}}}, {"to", std::move(rcpts)}}}};
  if (stage == "data") {
    bj::array headers;
    size_t pos = 0;
    while (pos < raw_headers.size()) {
      size_t nl = raw_headers.find("\r\n", pos);
      if (nl == std::string::npos) nl = raw_headers.size();
      std::string line = raw_headers.substr(pos, nl - pos);
      auto colon = line.find(':');
      headers.push_back(bj::array{line.substr(0, colon), line.substr(colon + 1) + "\r\n"});
      pos = nl + 2;
    }
    o["message"] = bj::object{{"headers", std::move(headers)}, {"contents", body},
                              {"size", int64_t(raw_headers.size() + 2 + body.size())}};
  }
  return json::dump(o);
}

}  // namespace

class MailInTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "mail_in";
    fs::remove_all(root);
    fs::create_directories(root);
    base::write_file_durable(root / "hook.token", kToken + "\n");
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"kcmeatup.com": {"tenant": "kc"}, "localhost": {"tenant": "kc"}},
        "mail": {"inbound": {"token-file": "hook.token"}},
        "operations": {"token": "op-token-for-mail-in-tests-0123456789"},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(cfg);
    port = host->start();
    ASSERT_TRUE(host->schemas().put(kSchema, &err)) << err;
    auto schema = host->schemas().get("got1");
    ASSERT_TRUE(host->databases().open("kc", "got", &*schema, &err)) << err;
    ASSERT_TRUE(host->boxes().upload(kDesk, kWat, &err)) << err;
  }
  void TearDown() override { host.reset(); }

  bj::object call(const std::string& body, std::vector<std::pair<std::string, std::string>> extra = {}) {
    extra.emplace_back("authorization", "Bearer " + kToken);
    auto r = post(port, "/internal/routes/mta/stalwart", body, extra);
    EXPECT_EQ(r.status, 200) << r.body;
    std::string err;
    auto v = json::parse(r.body, &err);
    return v && v->is_object() ? v->as_object() : bj::object{};
  }
  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = post(port, "/goober-operations-mcp", json::dump(req),
                  {{"authorization", "Bearer op-token-for-mail-in-tests-0123456789"}, {"content-type", "application/json"}});
    EXPECT_EQ(r.status, 200) << r.body;
    std::string err;
    return json::parse(r.body, &err)->at("result").at("structuredContent");
  }
  std::string action(const bj::object& o) { return o.contains("action") ? std::string(o.at("action").as_string()) : ""; }
  int64_t status(const bj::object& o) { return o.contains("response") ? o.at("response").at("status").as_int64() : 0; }
  bj::array rows() {
    std::string err;
    auto r = host->databases().operate("kc", "got", "SELECT path, body FROM got ORDER BY rowid", nullptr, 100, false, &err);
    return r ? r->at("rows").as_array() : bj::array{};
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(MailInTest, OnlyStalwartOnThisMachineWithItsToken) {
  std::string body = hook("rcpt", {"support@kcmeatup.com"});
  EXPECT_EQ(post(port, "/internal/routes/mta/stalwart", body, {}).status, 404);
  EXPECT_EQ(post(port, "/internal/routes/mta/stalwart", body, {{"authorization", "Bearer wrong"}}).status, 404);
  // Through Caddy (any proxy header), even with the token.
  EXPECT_EQ(post(port, "/internal/routes/mta/stalwart", body,
                 {{"authorization", "Bearer " + kToken}, {"x-forwarded-for", "203.0.113.9"}})
                .status,
            404);
  EXPECT_EQ(action(call(body)), "accept");
}

TEST_F(MailInTest, RecipientsAreRefusedAtTheDoor) {
  EXPECT_EQ(action(call(hook("rcpt", {"support@kcmeatup.com"}))), "accept");
  EXPECT_EQ(action(call(hook("rcpt", {"later@kcmeatup.com"}))), "accept");
  auto nobody = call(hook("rcpt", {"nobody@kcmeatup.com"}));
  EXPECT_EQ(action(nobody), "reject");
  EXPECT_EQ(status(nobody), 550);
  EXPECT_EQ(nobody.at("response").at("enhanced_status"), "5.1.1");
  EXPECT_EQ(status(call(hook("rcpt", {"support@elsewhere.example"}))), 550);
  EXPECT_EQ(status(call(hook("rcpt", {"not an address"}))), 550);
  // One recipient per message: the second is asked to come separately.
  auto second = call(hook("rcpt", {"support@kcmeatup.com", "refuse@kcmeatup.com"}));
  EXPECT_EQ(status(second), 452);
  // Other stages pass.
  EXPECT_EQ(action(call(hook("ehlo", {}))), "accept");
}

TEST_F(MailInTest, TheBoxsAnswerIsTheSmtpAnswer) {
  std::string headers = "From: Ann Lee <ann@example.org>\r\nTo: support@kcmeatup.com\r\nSubject: Half a cow?\r\n"
                        "Message-ID: <m1@example.org>";
  auto taken = call(hook("data", {"support@kcmeatup.com"}, headers, "Do you have half a cow left?\r\n"));
  EXPECT_EQ(action(taken), "discard");  // 250, and Stalwart keeps nothing
  auto got = rows();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].at(0), "mail:support@kcmeatup.com");
  std::string err;
  auto typed = json::parse(std::string(got[0].at(1).as_string()), &err)->as_object();
  EXPECT_EQ(typed.at("subject"), "Half a cow?");
  EXPECT_EQ(typed.at("from").at("address"), "ann@example.org");
  EXPECT_EQ(typed.at("from").at("name"), "Ann Lee");
  EXPECT_EQ(typed.at("to"), "support@kcmeatup.com");
  EXPECT_EQ(typed.at("text"), "Do you have half a cow left?\r\n");
  EXPECT_EQ(typed.at("message_id"), "m1@example.org");
  EXPECT_TRUE(typed.at("sender").is_null());  // not a verified claim
  EXPECT_FALSE(typed.at("automatic").as_bool());
  // The host noted who wrote and which message, as hashes, so an answer in this thread counts
  // as a reply (018 § 3), and kept neither the address nor the Message-ID.
  {
    sqlite::Db inbound(root / "storage/mail/inbound.db");
    sqlite::Stmt s(inbound, "SELECT tenant, sender, message FROM received");
    ASSERT_TRUE(s.step());
    EXPECT_EQ(s.text(1), host::received_hash("ann@example.org"));
    EXPECT_EQ(s.text(2), host::received_hash("m1@example.org"));
    EXPECT_EQ(s.text(1).find("ann"), std::string::npos);
  }

  auto no = call(hook("data", {"refuse@kcmeatup.com"}, "Subject: hi", "hi"));
  EXPECT_EQ(status(no), 550);
  EXPECT_EQ(no.at("response").at("message"), "No thanks");
  auto later = call(hook("data", {"later@kcmeatup.com"}, "Subject: hi", "hi"));
  EXPECT_EQ(status(later), 451);
  EXPECT_EQ(rows().size(), 1u);

  // An agent can see it all without the server: what came in for the tenant, and the answers.
  auto seen = tool("mail_activity", {{"tenant", "kc"}});
  const auto& mail = seen.at("mail").as_array();
  std::string outcomes;
  for (const auto& m : mail) {
    EXPECT_EQ(m.at("direction"), "in");
    outcomes += std::string(m.at("to").as_string()) + "=" + std::string(m.at("outcome").as_string()) + " ";
  }
  EXPECT_NE(outcomes.find("support@kcmeatup.com=delivered"), std::string::npos) << outcomes;
  EXPECT_NE(outcomes.find("later@kcmeatup.com="), std::string::npos) << outcomes;
  EXPECT_TRUE(tool("mail_activity", {{"tenant", "other"}}).at("mail").as_array().empty());
}

TEST_F(MailInTest, LimitsLoopsAndAttachments) {
  // An auto-reply is taken and dropped, never reaching the box.
  auto auto_reply = call(hook("data", {"support@kcmeatup.com"}, "Subject: Out of office\r\nAuto-Submitted: auto-replied", "away"));
  EXPECT_EQ(action(auto_reply), "discard");
  // So is a bounce (an empty envelope sender).
  EXPECT_EQ(action(call(hook("data", {"support@kcmeatup.com"}, "Subject: Undeliverable", "x", ""))), "discard");
  EXPECT_TRUE(rows().empty());

  // Past the route's max-bytes (4096).
  auto big = call(hook("data", {"support@kcmeatup.com"}, "Subject: big", std::string(5000, 'x')));
  EXPECT_EQ(status(big), 552);

  std::string mixed = "Subject: menu\r\nContent-Type: multipart/mixed; boundary=b";
  std::string one = "--b\r\nContent-Type: text/plain\r\n\r\nSee attached.\r\n--b\r\nContent-Type: application/pdf\r\n"
                    "Content-Disposition: attachment; filename=menu.pdf\r\nContent-Transfer-Encoding: base64\r\n\r\n"
                    "JVBERi0xLjQK\r\n--b--\r\n";
  EXPECT_EQ(action(call(hook("data", {"support@kcmeatup.com"}, mixed, one))), "discard");
  auto got = rows();
  ASSERT_EQ(got.size(), 1u);
  std::string err;
  auto typed = json::parse(std::string(got[0].at(1).as_string()), &err)->as_object();
  EXPECT_EQ(typed.at("text"), "See attached.");
  ASSERT_EQ(typed.at("attachments").as_array().size(), 1u);
  EXPECT_EQ(typed.at("attachments").at(0).at("filename"), "menu.pdf");
  EXPECT_EQ(typed.at("attachments").at(0).at("bytes"), 9);
  // Two attachments, where the route takes one: refused.
  std::string two = one.substr(0, one.size() - 6) + "\r\n--b\r\nContent-Type: image/png\r\nContent-Disposition: attachment; "
                    "filename=a.png\r\n\r\nx\r\n--b--\r\n";
  EXPECT_EQ(status(call(hook("data", {"support@kcmeatup.com"}, mixed, two))), 550);
  // A route that takes none says so.
  EXPECT_EQ(call(hook("data", {"refuse@kcmeatup.com"}, mixed, one)).at("response").at("message"),
            "This address takes no attachments");
}

TEST_F(MailInTest, ReplyAddressesAreCapabilities) {
  // The box sends mail and asks for a reply address on its token route.
  asio::io_context io;
  auto r = post(port, "/notify", "{}", {{"host", "kcmeatup.com"}});
  ASSERT_EQ(r.status, 200) << r.body;
  std::string err;
  auto sent = json::parse(r.body, &err)->as_object();
  std::string reply(sent.at("reply_to").as_string());
  ASSERT_TRUE(reply.starts_with("reply+")) << reply;
  ASSERT_TRUE(reply.ends_with("@kcmeatup.com")) << reply;

  std::string why;
  auto m = host->match_mail(reply, &why);
  ASSERT_TRUE(m) << why;
  EXPECT_EQ(m->params.at("ticket"), "42");  // the claim, not the token
  EXPECT_EQ(m->ref.route->name, "ticket_reply");
  EXPECT_EQ(m->reply_recipient, "cy@example.org");

  EXPECT_EQ(action(call(hook("rcpt", {reply}))), "accept");
  // A token changed by one character is refused at the door.
  std::string forged = reply;
  forged[7] = forged[7] == 'a' ? 'b' : 'a';
  EXPECT_EQ(status(call(hook("rcpt", {forged}))), 550);
  EXPECT_FALSE(host->match_mail("reply+aaaaaaaaaaaaaaaaaaaaaaaa@kcmeatup.com", &why));

  EXPECT_EQ(action(call(hook("data", {reply}, "From: cy@example.org\r\nSubject: Re: About ticket 42", "Yes."))), "discard");
  auto got = rows();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(std::string(got[0].at(0).as_string()), "mail:" + reply);
}

TEST_F(MailInTest, ACatchAllTakesWhatNothingElseClaims) {
  std::string err;
  ASSERT_TRUE(host->boxes().upload(kInbox, kWat, &err)) << err;
  EXPECT_EQ(action(call(hook("rcpt", {"anyone@kcmeatup.com"}))), "accept");
  std::string why;
  EXPECT_EQ(host->match_mail("support@kcmeatup.com", &why)->ref.route->name, "support_inbox");  // specific first
  EXPECT_EQ(host->match_mail("anyone@kcmeatup.com", &why)->ref.route->name, "inbox_all");
  // This route opted in to automatic mail.
  EXPECT_EQ(action(call(hook("data", {"anyone@kcmeatup.com"}, "Subject: Out\r\nAuto-Submitted: auto-replied", "away"))),
            "discard");
  auto got = rows();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].at(0), "mail:anyone@kcmeatup.com");
  auto typed = json::parse(std::string(got[0].at(1).as_string()), &err)->as_object();
  EXPECT_TRUE(typed.at("automatic").as_bool());
}
