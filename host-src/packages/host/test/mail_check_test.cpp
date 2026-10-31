// Mail checks for operators (mail_check.cpp, dns.cpp): DNS answers parsed from wire-format bytes,
// the SMTP banner probe, the listener in host_health, mail_route_check, mail routes in box_list,
// mail_domain_check's bounds, and test sends that show in mail_activity.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <thread>

#include "../src/dns.h"
#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;
namespace dns = goober::host::dns;

namespace {

// A DNS response: header, one question for `qname`, then the answers given as (type, rdata).
std::vector<uint8_t> response(const std::string& qname, int qtype, const std::vector<std::pair<int, std::vector<uint8_t>>>& answers) {
  std::vector<uint8_t> m{0x12, 0x34, 0x81, 0x80, 0, 1, 0, uint8_t(answers.size()), 0, 0, 0, 0};
  size_t start = 0;
  while (start <= qname.size()) {
    size_t dot = qname.find('.', start);
    if (dot == std::string::npos) dot = qname.size();
    m.push_back(uint8_t(dot - start));
    m.insert(m.end(), qname.begin() + long(start), qname.begin() + long(dot));
    start = dot + 1;
  }
  m.push_back(0);
  m.insert(m.end(), {0, uint8_t(qtype), 0, 1});
  for (const auto& [type, rdata] : answers) {
    m.insert(m.end(), {0xC0, 0x0C, 0, uint8_t(type), 0, 1, 0, 0, 0x0E, 0x10, uint8_t(rdata.size() >> 8), uint8_t(rdata.size())});
    m.insert(m.end(), rdata.begin(), rdata.end());
  }
  return m;
}

std::vector<uint8_t> label_name(const std::string& name) {
  std::vector<uint8_t> out;
  size_t start = 0;
  while (start < name.size()) {
    size_t dot = name.find('.', start);
    if (dot == std::string::npos) dot = name.size();
    out.push_back(uint8_t(dot - start));
    out.insert(out.end(), name.begin() + long(start), name.begin() + long(dot));
    start = dot + 1;
  }
  out.push_back(0);
  return out;
}

// A stand-in SMTP server: answers every connection with a banner, then closes.
class FakeSmtp {
 public:
  FakeSmtp() : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] {
      for (;;) {
        boost::system::error_code ec;
        asio::ip::tcp::socket s(io_);
        acceptor_.accept(s, ec);
        if (ec || stop_) return;
        asio::write(s, asio::buffer(std::string("220 mx.test ESMTP ready\r\n")), ec);
        char buf[64];
        s.read_some(asio::buffer(buf), ec);  // the probe's QUIT
      }
    });
  }
  ~FakeSmtp() {
    stop_ = true;
    boost::system::error_code ec;
    asio::ip::tcp::socket poke(io_);
    poke.connect(acceptor_.local_endpoint(), ec);
    thread_.join();
  }
  uint16_t port() const { return port_; }

 private:
  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  uint16_t port_ = 0;
};

const char* kWat = R"wat((module
  (memory (export "memory") 1)
  (func (export "goober_route_support_inbox") (result i32) (i32.const 0))
  (func (export "goober_route_inbox_all") (result i32) (i32.const 0))
))wat";

const char* kDesk = R"({"box":"desk","domains":["shop.test"],
  "routes":{"mail":{"support@":{"receive":{"name":"support_inbox","public":true}},
                    "*@":{"receive":{"name":"inbox_all","public":true}}}}})";

}  // namespace

TEST(Dns, ParsesMxAndTxtAnswers) {
  // MX: two records, the second's exchange name compressed against the question.
  std::vector<uint8_t> mx1{0, 20};
  auto n1 = label_name("backup.example.com");
  mx1.insert(mx1.end(), n1.begin(), n1.end());
  std::vector<uint8_t> mx2{0, 10, 2, 'm', 'x', 0xC0, 0x0C};  // mx. + the question's example.com
  std::string err;
  auto mx = dns::parse_mx(response("Example.com", 15, {{15, mx1}, {15, mx2}}), &err);
  ASSERT_TRUE(mx) << err;
  ASSERT_EQ(mx->size(), 2u);
  EXPECT_EQ((*mx)[0].preference, 10);  // sorted by preference
  EXPECT_EQ((*mx)[0].host, "mx.example.com");
  EXPECT_EQ((*mx)[1].host, "backup.example.com");

  // TXT: one record of two strings (joined), one of one; a record of another type is skipped.
  std::vector<uint8_t> spf{12, 'v', '=', 's', 'p', 'f', '1', ' ', 'i', 'n', 'c', 'l', ':', 3, 'x', ' ', 'y'};
  std::vector<uint8_t> other{5, 'h', 'e', 'l', 'l', 'o'};
  auto txt = dns::parse_txt(response("example.com", 16, {{16, spf}, {1, {1, 2, 3, 4}}, {16, other}}), &err);
  ASSERT_TRUE(txt) << err;
  EXPECT_EQ(*txt, (std::vector<std::string>{"v=spf1 incl:x y", "hello"}));

  // Malformed: cut short, a pointer loop, a string past its record.
  auto cut = response("example.com", 15, {{15, mx1}});
  cut.resize(cut.size() - 3);
  EXPECT_FALSE(dns::parse_mx(cut, &err));
  std::vector<uint8_t> loop{0, 10, 0xC0, 0};  // points at the header, whose bytes aren't a name
  auto looped = response("example.com", 15, {{15, loop}});
  looped[0] = 0xC0;
  looped[1] = 0x00;  // the header itself now points at itself
  EXPECT_FALSE(dns::parse_mx(looped, &err));
  EXPECT_FALSE(dns::parse_txt(response("example.com", 16, {{16, {9, 'a'}}}), &err));
  EXPECT_FALSE(dns::parse_txt({1, 2, 3}, &err));
}

TEST(Dns, SmtpBannerReadsTheFirstLineAndNothingElse) {
  FakeSmtp smtp;
  std::string err;
  auto banner = dns::smtp_banner("127.0.0.1", smtp.port(), 2000, &err);
  ASSERT_TRUE(banner) << err;
  EXPECT_EQ(*banner, "220 mx.test ESMTP ready");
  // Nothing listening: refused, quickly, with why.
  asio::io_context io;
  asio::ip::tcp::acceptor a(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  uint16_t closed = a.local_endpoint().port();
  a.close();
  EXPECT_FALSE(dns::smtp_banner("127.0.0.1", closed, 1000, &err));
  EXPECT_FALSE(err.empty());
  EXPECT_FALSE(dns::smtp_banner("not-an-address", 25, 1000, &err));
}

class MailCheckTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "mail_check";
    fs::remove_all(root);
    fs::create_directories(root);
    base::write_file_durable(root / "hook.token", "mail-check-test-token-0123456789abcdef\n");
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"shop.test": {"tenant": "shop"}, "localhost": {"tenant": "shop"}, "blog.test": {"tenant": "blog"}},
        "tenants": {"shop": {"postal-address": "Shop, 1 Main St, Springfield"}},
        "mail": {"inbound": {"token-file": "hook.token"}},
        "operations": {"token": "op-token-for-mail-check-0123456789"},
        "threads": {"network": 1, "boxes": 1}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(cfg);
    host->set_smtp_listener_port(smtp.port());
    port = host->start();
    ASSERT_TRUE(host->boxes().upload(kDesk, kWat, &err)) << err;
  }
  void TearDown() override { host.reset(); }

  bj::value tool(const std::string& name, bj::object args) {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    bhttp::request<bhttp::string_body> req(bhttp::verb::post, "/goober-operations-mcp", 11);
    req.set(bhttp::field::host, "localhost");
    req.set(bhttp::field::content_type, "application/json");
    req.set(bhttp::field::authorization, "Bearer op-token-for-mail-check-0123456789");
    req.body() = json::dump(bj::object{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                       {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}});
    req.prepare_payload();
    bhttp::write(stream, req);
    beast::flat_buffer buf;
    bhttp::response<bhttp::string_body> res;
    bhttp::read(stream, buf, res);
    std::string err;
    auto doc = json::parse(res.body(), &err);
    const auto& result = doc->at("result");
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::object{{"error", text}};
    return *json::parse(text, &err);
  }

  FakeSmtp smtp;
  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(MailCheckTest, HealthSaysWhetherTheListenerAnswers) {
  auto health = tool("host_health", {});
  const auto& l = health.at("smtp_listener");
  EXPECT_EQ(l.at("configured"), true);
  EXPECT_EQ(l.at("up"), true) << json::dump(l);
  EXPECT_EQ(l.at("banner"), "220 mx.test ESMTP ready");
  // blog has no postal address: health says who sets one, and that no tool does.
  EXPECT_EQ(health.at("tenants_without_postal_address").at(0), "blog");
  EXPECT_NE(std::string(health.at("postal_address_how").as_string()).find("no tool sets it"), std::string::npos);
  // Nothing listening: down, with why.
  host->set_smtp_listener_port(1);
  auto down = tool("host_health", {}).at("smtp_listener");
  EXPECT_EQ(down.at("up"), false);
  EXPECT_TRUE(down.as_object().contains("error"));
}

// Moving a mail route (and its route name) from one box to another, with a quick rollback: the
// new box is stored dark first (its conflict with the live box is a warning), then the cutover
// is box_disable on the old box and version_promote on the new; rollback is the reverse.
TEST_F(MailCheckTest, AMailRouteMovesBetweenBoxesAndBack) {
  const char* kAgentWat = R"wat((module
    (memory (export "memory") 1)
    (func (export "goober_route_support_inbox") (result i32) (i32.const 0))))wat";
  bj::object agent_manifest{
      {"box", "agent"},
      {"domains", bj::array{"shop.test"}},
      {"routes", bj::object{{"mail", bj::object{{"support@", bj::object{{"receive", bj::object{
                                                                 {"name", "support_inbox"}, {"public", true}}}}}}}}}};
  // While desk is live, agent can't go live: the name and the address are desk's.
  auto live = tool("version_upload", {{"manifest", agent_manifest}, {"wat", kAgentWat}});
  ASSERT_TRUE(live.as_object().contains("error"));
  EXPECT_NE(std::string(live.at("error").as_string()).find("already used by box desk"), std::string::npos);
  // Stored dark, with the conflict as a warning.
  auto dark = tool("version_upload", {{"manifest", agent_manifest}, {"wat", kAgentWat}, {"activate", false}});
  ASSERT_FALSE(dark.as_object().contains("error")) << json::dump(dark);
  EXPECT_NE(json::dump(dark.at("warnings")).find("box_disable the other box first"), std::string::npos) << json::dump(dark);
  std::string v(dark.at("version").as_string());
  EXPECT_EQ(tool("mail_route_check", {{"domain", "shop.test"}, {"address", "support"}}).at("box"), "desk");
  // Promoting it while desk holds the address is refused, saying by whom.
  EXPECT_TRUE(tool("version_promote", {{"box", "agent"}, {"version", v}}).as_object().contains("error"));

  // The cutover: two calls. A disabled box holds no names or addresses.
  ASSERT_FALSE(tool("box_disable", {{"box", "desk"}, {"why", "support@ moves to agent"}}).as_object().contains("error"));
  auto promoted = tool("version_promote", {{"box", "agent"}, {"version", v}});
  ASSERT_FALSE(promoted.as_object().contains("error")) << json::dump(promoted);
  auto now = tool("mail_route_check", {{"domain", "shop.test"}, {"address", "support"}});
  EXPECT_EQ(now.at("box"), "agent");
  EXPECT_EQ(now.at("route"), "support_inbox");

  // desk can't come back while agent holds them, and the refusal says how to roll back.
  auto blocked = tool("box_enable", {{"box", "desk"}});
  ASSERT_TRUE(blocked.as_object().contains("error"));
  EXPECT_NE(std::string(blocked.at("error").as_string()).find("box_disable that box first"), std::string::npos)
      << json::dump(blocked);
  // Rollback: two calls the other way.
  ASSERT_FALSE(tool("box_disable", {{"box", "agent"}, {"why", "rollback"}}).as_object().contains("error"));
  ASSERT_FALSE(tool("box_enable", {{"box", "desk"}}).as_object().contains("error"));
  EXPECT_EQ(tool("mail_route_check", {{"domain", "shop.test"}, {"address", "support"}}).at("box"), "desk");
  EXPECT_EQ(tool("mail_route_check", {{"domain", "shop.test"}, {"address", "sales"}}).at("route"), "inbox_all");
}

TEST_F(MailCheckTest, RouteCheckSaysWhoTakesAnAddress) {
  auto support = tool("mail_route_check", {{"domain", "shop.test"}, {"address", "Support"}});
  EXPECT_EQ(support.at("taken"), true) << json::dump(support);
  EXPECT_EQ(support.at("box"), "desk");
  EXPECT_EQ(support.at("route"), "support_inbox");
  EXPECT_EQ(support.at("catch_all"), false);
  EXPECT_EQ(support.at("address"), "support@shop.test");
  auto other = tool("mail_route_check", {{"domain", "shop.test"}, {"address", "sales@shop.test"}});
  EXPECT_EQ(other.at("route"), "inbox_all");
  EXPECT_EQ(other.at("catch_all"), true);
  // The box serves shop.test only: blog.test's mail is refused at RCPT, saying why.
  auto blog = tool("mail_route_check", {{"domain", "blog.test"}, {"address", "support"}});
  EXPECT_EQ(blog.at("taken"), false);
  EXPECT_EQ(blog.at("refused_at"), "RCPT");
  EXPECT_EQ(blog.at("why"), "no mail route claims it");
  EXPECT_TRUE(tool("mail_route_check", {{"domain", "shop.test"}, {"address", "a@blog.test"}}).as_object().contains("error"));
  EXPECT_TRUE(tool("mail_route_check", {{"domain", "nope.test"}, {"address", "a"}}).as_object().contains("error"));
  // box_list names the addresses each box takes.
  auto boxes = tool("box_list", {});
  ASSERT_EQ(boxes.as_array().size(), 1u);
  EXPECT_EQ(json::dump(boxes.at(0).at("mail")),
            R"([{"address":"support@","name":"support_inbox"},{"address":"*@","name":"inbox_all"}])");
}

TEST_F(MailCheckTest, DomainCheckKeepsToTheHostsOwnDomains) {
  EXPECT_TRUE(tool("mail_domain_check", {{"domain", "elsewhere.example"}}).as_object().contains("error"));
  // A tenant domain: answered whatever DNS says (a test machine may have no network), with the
  // mailer and SES's part said plainly.
  auto shop = tool("mail_domain_check", {{"domain", "shop.test"}});
  ASSERT_FALSE(shop.as_object().contains("error")) << json::dump(shop);
  EXPECT_EQ(shop.at("tenant"), "shop");
  EXPECT_EQ(shop.at("mailer"), "dev");
  EXPECT_NE(json::dump(shop.at("ses")).find("not SES"), std::string::npos);
  EXPECT_TRUE(shop.at("findings").is_array());
  // A tenant without a postal address hears how it gets one.
  auto blog = tool("mail_domain_check", {{"domain", "blog.test"}});
  EXPECT_NE(json::dump(blog.at("findings")).find("no tool sets it"), std::string::npos) << json::dump(blog);
}

TEST_F(MailCheckTest, TestSendsShowInMailActivity) {
  auto sent = tool("mail_template_send_test", {{"tenant", "shop"}, {"name", "sign-in-new"}, {"to", "ops@example.org"}});
  ASSERT_FALSE(sent.as_object().contains("error")) << json::dump(sent);
  // The mail queue sends on its own thread: wait for the log line.
  bj::array mail;
  for (int i = 0; i < 50 && mail.empty(); ++i) {
    mail = tool("mail_activity", {{"tenant", "shop"}, {"email", "ops@example.org"}}).at("mail").as_array();
    if (mail.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_EQ(mail.size(), 1u);
  EXPECT_EQ(mail[0].at("direction"), "out");
  EXPECT_EQ(mail[0].at("test"), true);
  EXPECT_EQ(mail[0].at("outcome"), "sent");
  EXPECT_EQ(mail[0].at("via"), "dev");
  EXPECT_EQ(std::string(mail[0].at("subject").as_string()).rfind("[Test] ", 0), 0u);
  // Another tenant's activity doesn't show it.
  EXPECT_TRUE(tool("mail_activity", {{"tenant", "blog"}}).at("mail").as_array().empty());
}
