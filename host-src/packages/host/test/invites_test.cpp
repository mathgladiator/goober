// Invites, consent, and mail preferences (design/machine/029.Invites.md): the host keeps the
// record and the template, accepting takes a code, declining blocks for good, and every box
// mail has a category with its own rule.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

struct Reply {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};

const std::string kSite = "kc.example.com";
const std::string kOrigin = "http://kc.example.com";
const std::string kOps = "ops.example.com";

Reply fetch(uint16_t port, const std::string& host, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, host);
  for (const auto& [k, v] : headers) req.set(k, v);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  Reply r{int(res.result_int()), res.body(), {}};
  for (const auto& f : res) r.headers[base::to_lower(std::string(f.name_string()))] = std::string(f.value());
  return r;
}

bj::value parse(const std::string& s) {
  std::string err;
  auto v = json::parse(s, &err);
  return v ? *v : bj::value(nullptr);
}

std::string cookie_of(const Reply& r) {
  auto it = r.headers.find("set-cookie");
  return it == r.headers.end() ? "" : it->second.substr(0, it->second.find(';'));
}

// A WAT module with one route per (name, service, method, request): each calls the service once
// and answers what the host said.
std::string calling_module(const std::vector<std::tuple<std::string, std::string, std::string, std::string>>& routes) {
  std::string data, funcs;
  int at = 64;
  auto put = [&](const std::string& s) {
    std::string escaped;
    for (char c : s) escaped += c == '"' ? std::string("\\22") : c == '\\' ? std::string("\\5c") : std::string(1, c);
    data += "  (data (i32.const " + std::to_string(at) + ") \"" + escaped + "\")\n";
    int start = at;
    at += int(s.size()) + 8;
    return std::pair{start, int(s.size())};
  };
  for (const auto& [name, service, method, request] : routes) {
    auto s = put(service), m = put(method), r = put(request);
    funcs += "  (func (export \"goober_route_" + name + "\") (result i32) (local $n i32)\n"
             "    (local.set $n (call $call (i32.const " + std::to_string(s.first) + ") (i32.const " + std::to_string(s.second) +
             ") (i32.const " + std::to_string(m.first) + ") (i32.const " + std::to_string(m.second) + ") (i32.const " +
             std::to_string(r.first) + ") (i32.const " + std::to_string(r.second) + ")))\n"
             "    (drop (call $result (i32.const 32768) (local.get $n)))\n"
             "    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 0) (i32.const 32768) (local.get $n)))\n"
             "    (i32.const 0))\n";
  }
  return "(module\n"
         "  (import \"goober\" \"goober_call\" (func $call (param i32 i32 i32 i32 i32 i32) (result i32)))\n"
         "  (import \"goober\" \"goober_result_read\" (func $result (param i32 i32) (result i32)))\n"
         "  (import \"goober\" \"goober_respond\" (func $respond (param i32 i32 i32 i32 i32) (result i32)))\n"
         "  (memory (export \"memory\") 1)\n"
         "  (data (i32.const 0) \"application/json\")\n" +
         data + funcs + ")\n";
}

}  // namespace

class InvitesHostTest : public ::testing::Test {
 protected:
  void start(const std::string& tenants) {
    root = fs::path(GOOBER_TEST_TMP) / "invites_host";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"kc.example.com": {"tenant": "kc"}, "other.example.com": {"tenant": "other"}},
        "operations": {"token": "op-token-for-invite-tests-0123456789", "domain": "ops.example.com"},
        "identity": {"operators": {"boss@example.com": ["host:owner"], "bot@example.com": ["host:operator"]}},
        "tenants": )" + tenants + R"(,
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  bj::object tool(const std::string& name, const std::string& args) {
    auto r = fetch(port, kSite, bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer op-token-for-invite-tests-0123456789"}, {"content-type", "application/json"}},
                   R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + name + R"(","arguments":)" + args + "}}");
    auto v = parse(r.body);
    auto res = v.as_object().at("result").as_object();
    bj::object out{{"isError", res.at("isError")}, {"text", res.at("content").as_array().at(0).at("text")}};
    if (res.contains("structuredContent")) out["value"] = res.at("structuredContent");
    return out;
  }
  // The newest mail to an address, as the dev mailer wrote it.
  std::string newest_mail(const std::string& to) {
    host->drain_mail();
    auto dir = root / "storage/mail/outbox";
    if (!fs::exists(dir)) return "";
    auto files = base::list_files(dir);
    std::sort(files.begin(), files.end());
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
      auto text = *base::read_file(dir / *it);
      if (text.find("To: " + to) != std::string::npos) return text;
    }
    return "";
  }
  int mails_to(const std::string& to) {
    host->drain_mail();
    auto dir = root / "storage/mail/outbox";
    if (!fs::exists(dir)) return 0;
    int n = 0;
    for (const auto& f : base::list_files(dir)) n += base::read_file(dir / f)->find("To: " + to) != std::string::npos;
    return n;
  }
  // The first link in the mail's body (after its headers) whose path starts with prefix: the path.
  static std::string link(const std::string& mail, const std::string& prefix) {
    std::smatch m;
    std::string flat = std::regex_replace(mail, std::regex("=\r\n"), "");
    flat = flat.substr(flat.find("\r\n\r\n"));
    if (!std::regex_search(flat, m, std::regex(prefix + "[A-Za-z0-9/._%-]+"))) return "";
    return m[0];
  }
  std::string code_in(const std::string& mail) {
    std::smatch m;
    return std::regex_search(mail, m, std::regex("is ([0-9]{6})")) ? m[1].str() : "";
  }
  Reply get(const std::string& path, const std::string& cookie = "") {
    std::vector<std::pair<std::string, std::string>> h;
    if (!cookie.empty()) h.emplace_back("cookie", cookie);
    return fetch(port, kSite, bhttp::verb::get, path, h);
  }
  Reply post_form(const std::string& path, const std::string& body, const std::string& cookie = "",
                  const std::string& origin = kOrigin) {
    std::vector<std::pair<std::string, std::string>> h{{"content-type", "application/x-www-form-urlencoded"}};
    if (!origin.empty()) h.emplace_back("origin", origin);
    if (!cookie.empty()) h.emplace_back("cookie", cookie);
    return fetch(port, kSite, bhttp::verb::post, path, h, body);
  }
  std::string sign_in(const std::string& email) {
    post_form("/identity/email/start", "email=" + email);
    auto code = code_in(newest_mail(email));
    return cookie_of(post_form("/identity/email/verify", "email=" + email + "&code=" + code));
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

const char* kTenants = R"({"kc": {"postal-address": "KC Meat Up, 1 Main St, Kansas City, MO 64105", "terms": "/terms",
    "privacy": "/privacy", "invites": {"roles": ["member"]}}})";

TEST_F(InvitesHostTest, AnOperatorsInviteIsAcceptedByCodeAndGrantsWhatItSaid) {
  start(kTenants);
  std::string err;
  ASSERT_TRUE(host->content().put("kc", "terms.md", "---\ntitle: Terms\n---\nBe kind.\n", &err)) << err;
  auto sent = tool("invite_send", R"({"tenant":"kc","email":"Bob@Example.com","roles":["member","editor"],
      "note":"Come grill with us","inviter_name":"Ann","return":"/welcome"})");
  ASSERT_FALSE(sent.at("isError").as_bool()) << sent.at("text");
  std::string id(sent.at("value").at("invite_id").as_string());
  std::string mail = newest_mail("bob@example.com");
  ASSERT_FALSE(mail.empty());
  // The host's mail: who invited, the note, both links, the footer, and the one-click decline.
  EXPECT_NE(mail.find("Ann invited you to kc.example.com"), std::string::npos) << mail;
  EXPECT_NE(mail.find("Come grill with us"), std::string::npos);
  EXPECT_NE(mail.find("KC Meat Up, 1 Main St"), std::string::npos);
  EXPECT_NE(mail.find("You'll get one reminder, then nothing."), std::string::npos);
  std::string path = link(mail, "/identity/invite/");
  ASSERT_NE(path.find("/identity/invite/" + id + "/"), std::string::npos) << path;
  EXPECT_NE(mail.find(path + "/decline>"), std::string::npos) << mail;  // List-Unsubscribe declines

  // Opening the link changes nothing: it shows who, what, and the terms.
  auto page = get(path);
  EXPECT_EQ(page.status, 200);
  EXPECT_NE(page.body.find("Ann invited <strong>bob@example.com</strong>"), std::string::npos) << page.body;
  EXPECT_NE(page.body.find("member, editor"), std::string::npos);
  EXPECT_NE(page.body.find("By continuing, you accept"), std::string::npos);
  EXPECT_EQ(parse(tool("invite_get", R"({"tenant":"kc","invite_id":")" + id + R"("})").at("text").as_string().c_str())
                .at("state"),
            "open");
  // Continue from another site is refused; from this one, a code goes to the invited address,
  // and nobody is signed in by the link alone.
  EXPECT_EQ(post_form(path, "", "", "https://evil.example").status, 403);
  auto cont = post_form(path, "");
  EXPECT_EQ(cont.status, 303);
  EXPECT_TRUE(cookie_of(cont).empty());
  std::string next = cont.headers["location"];
  ASSERT_NE(next.find("/identity/sign-in/code?email=bob%40example.com&invite=" + id + "."), std::string::npos) << next;
  auto code_page = get(next);
  EXPECT_NE(code_page.body.find("By continuing, you accept"), std::string::npos) << code_page.body;
  std::smatch m;
  ASSERT_TRUE(std::regex_search(code_page.body, m, std::regex("name=\"invite\" value=\"([^\"]+)\"")));
  std::string token = m[1];
  auto code = code_in(newest_mail("bob@example.com"));
  auto done = post_form("/identity/email/verify", "email=bob%40example.com&code=" + code + "&invite=" + token + "&return=%2F");
  EXPECT_EQ(done.status, 303);
  EXPECT_EQ(done.headers["location"], "/welcome");
  std::string cookie = cookie_of(done);
  ASSERT_FALSE(cookie.empty());
  // Roles from an operator's invite are granted as written; terms are recorded with the page's hash.
  auto me = parse(fetch(port, kSite, bhttp::verb::get, "/identity/me", {{"cookie", cookie}}).body).as_object();
  EXPECT_EQ(me.at("roles").as_array().size(), 2u);
  auto user = host->identity().user("kc", std::stoll(std::string(me.at("user_id").as_string())));
  ASSERT_TRUE(user);
  EXPECT_TRUE(host->identity().terms_accepted("kc", user->user_id, "/terms").has_value());
  auto after = parse(tool("invite_get", R"({"tenant":"kc","invite_id":")" + id + R"("})").at("text").as_string().c_str());
  EXPECT_EQ(after.at("state"), "accepted");
  EXPECT_EQ(after.at("answer_how"), "code");
  EXPECT_EQ(get(path).status, 410);  // answered: the page says so, and offers nothing
  // An account can't be invited again.
  auto again = tool("invite_send", R"({"tenant":"kc","email":"bob@example.com"})");
  EXPECT_TRUE(again.at("isError").as_bool());
  EXPECT_NE(std::string(again.at("text").as_string()).find("member"), std::string::npos);
}

TEST_F(InvitesHostTest, DecliningBlocksTheTenantUntilAPersonLiftsIt) {
  start(kTenants);
  auto sent = tool("invite_send", R"({"tenant":"kc","email":"cy@example.com"})");
  ASSERT_FALSE(sent.at("isError").as_bool()) << sent.at("text");
  std::string decline = link(newest_mail("cy@example.com"), "/identity/invite/") + "/decline";
  auto asked = get(decline);
  EXPECT_NE(asked.body.find("Don't invite me to kc.example.com again?"), std::string::npos) << asked.body;
  EXPECT_FALSE(host->mail_suppressed("kc", "cy@example.com"));  // a GET changes nothing
  // A mail provider's one-click POST: no cookie, no Origin.
  auto one = fetch(port, kSite, bhttp::verb::post, decline, {{"content-type", "application/x-www-form-urlencoded"}},
                   "List-Unsubscribe=One-Click");
  EXPECT_EQ(one.status, 200);
  EXPECT_TRUE(host->mail_suppressed("kc", "cy@example.com"));
  EXPECT_FALSE(host->mail_suppressed("other", "cy@example.com"));  // one tenant only
  auto refused = tool("invite_send", R"({"tenant":"kc","email":"cy@example.com"})");
  EXPECT_TRUE(refused.at("isError").as_bool());
  EXPECT_NE(std::string(refused.at("text").as_string()).find("blocked"), std::string::npos);
  auto prefs = tool("mail_preferences", R"({"tenant":"kc","email":"cy@example.com"})");
  EXPECT_EQ(prefs.at("value").at("blocked").at("how"), "invite_declined");

  // No tool lifts it.
  auto shim = tool("mail_unsubscribed", R"({"tenant":"kc","email":"cy@example.com","set":false})");
  EXPECT_TRUE(shim.at("isError").as_bool());
  EXPECT_TRUE(host->mail_suppressed("kc", "cy@example.com"));
  // The person can, signed in, with the statement; the account page shows the block.
  std::string cookie = sign_in("cy@example.com");
  ASSERT_FALSE(cookie.empty());
  auto account = get("/identity/account", cookie);
  EXPECT_NE(account.body.find("not to email you"), std::string::npos) << account.body;
  EXPECT_EQ(post_form("/identity/mail/allow", "", cookie).status, 400);  // no statement: not lifted
  EXPECT_TRUE(host->mail_suppressed("kc", "cy@example.com"));
  auto allow = post_form("/identity/mail/allow", "statement=I%27m+asking+kc.example.com+to+email+me+again.", cookie);
  EXPECT_EQ(allow.status, 303);
  EXPECT_FALSE(host->mail_suppressed("kc", "cy@example.com"));
  auto history = tool("mail_preferences", R"({"tenant":"kc","email":"cy@example.com"})").at("value").at("history").as_array();
  ASSERT_GE(history.size(), 2u);
  EXPECT_EQ(history.back().at("what"), "lift");
  EXPECT_EQ(history.back().at("how"), "person");
}

TEST_F(InvitesHostTest, AnOperatorLiftsABlockOnlyInThePanelSayingWhy) {
  start(kTenants);
  tool("mail_unsubscribed", R"({"tenant":"kc","email":"dee@example.com","set":true})");
  ASSERT_TRUE(host->mail_suppressed("kc", "dee@example.com"));
  // Sign in on the panel's origin as the owner.
  auto start_code = fetch(port, kOps, bhttp::verb::post, "/identity/email/start",
                          {{"content-type", "application/x-www-form-urlencoded"}}, "email=boss@example.com");
  ASSERT_EQ(start_code.status, 200);
  auto code = code_in(newest_mail("boss@example.com"));
  auto v = fetch(port, kOps, bhttp::verb::post, "/identity/email/verify", {{"content-type", "application/x-www-form-urlencoded"}},
                 "email=boss@example.com&code=" + code);
  std::string boss = cookie_of(v);
  auto api = [&](const std::string& body) {
    return fetch(port, kOps, bhttp::verb::post, "/api/write/unblock",
                 {{"cookie", boss}, {"content-type", "application/json"}, {"origin", "http://" + kOps}}, body);
  };
  EXPECT_EQ(api(R"({"tenant":"kc","email":"dee@example.com","statement":"asked","confirm":true})").status, 400);  // too short
  EXPECT_EQ(api(R"({"tenant":"kc","email":"dee@example.com","statement":"Asked by phone on 11/2 to rejoin"})").status, 400);
  auto ok = api(R"({"tenant":"kc","email":"dee@example.com","statement":"Asked by phone on 11/2 to rejoin","confirm":true})");
  EXPECT_EQ(ok.status, 200) << ok.body;
  EXPECT_EQ(parse(ok.body).at("result").at("lifted"), true);
  EXPECT_FALSE(host->mail_suppressed("kc", "dee@example.com"));
}

TEST_F(InvitesHostTest, ATenantTemplateCantDropTheLinks) {
  start(kTenants);
  std::string err;
  ASSERT_TRUE(host->mail_templates().put("kc", "invite", {"Join us", "Come to {{site}}!", ""}, &err)) << err;
  ASSERT_FALSE(tool("invite_send", R"({"tenant":"kc","email":"eve@example.com"})").at("isError").as_bool());
  std::string mail = newest_mail("eve@example.com");
  EXPECT_EQ(mail.find("Subject: Join us"), std::string::npos) << mail;  // the default went instead
  EXPECT_NE(mail.find("/identity/invite/"), std::string::npos);
  EXPECT_NE(mail.find("/decline"), std::string::npos);
}

TEST_F(InvitesHostTest, NoPostalAddressNoInvites) {
  start(R"({"kc": {"invites": {"roles": ["member"]}}})");
  auto r = tool("invite_send", R"({"tenant":"kc","email":"fay@example.com"})");
  EXPECT_TRUE(r.at("isError").as_bool());
  EXPECT_NE(std::string(r.at("text").as_string()).find("no_postal_address"), std::string::npos) << r.at("text");
  EXPECT_EQ(mails_to("fay@example.com"), 0);
}

TEST_F(InvitesHostTest, OneReminderAfterAWeek) {
  start(kTenants);
  ASSERT_FALSE(tool("invite_send", R"({"tenant":"kc","email":"gil@example.com"})").at("isError").as_bool());
  EXPECT_EQ(mails_to("gil@example.com"), 1);
  int64_t now = base::now_unix_ms();
  host->identity_tick(now + 8LL * 24 * 3600 * 1000);
  EXPECT_EQ(mails_to("gil@example.com"), 2);
  EXPECT_NE(newest_mail("gil@example.com").find("This is the one reminder"), std::string::npos);
  host->identity_tick(now + 20LL * 24 * 3600 * 1000);  // a day's prune later: never a third
  EXPECT_EQ(mails_to("gil@example.com"), 2);
}

// The club box: an invites service with declared roles and a list, and routes that each make one call.
void upload_club(host::Host& host) {
  std::string module = calling_module({
      {"inv", "club", "send", R"({"email":"hal@example.com","roles":["member"],"lists":["digest"],"note":"Hi"})"},
      {"grab", "club", "send", R"({"email":"ivy@example.com","roles":["owner"]})"},
      {"named", "club", "send", R"({"email":"ivy@example.com","inviter_name":"Ann"})"},
      {"status", "club", "status", R"({"email":"hal@example.com"})"},
      {"sub", "mail", "subscribe", R"({"email":"jo@example.com","list":"digest"})"},
      {"digest", "mail", "send", R"({"to":"jo@example.com","subject":"Today","text":"News.","category":"list","list":"digest"})"},
      {"cold", "mail", "send", R"({"to":"kim@example.com","subject":"Hi","text":"Hello.","category":"transactional"})"},
      {"prefs", "mail", "preferences", R"({"email":"jo@example.com"})"},
  });
  std::string err;
  ASSERT_TRUE(host.boxes().upload(R"({"box":"club","domains":["kc.example.com"],
      "external":{"mail":{"mail":{"from-name":"KC Meat Up","lists":{"digest":{"title":"The daily digest","about":"One email a day."}}}},
                  "club":{"invites":{"roles":["member"],"lists":["digest"]}}},
      "routes":{"http":{"/inv":{"post":{"name":"inv","public":true}},"/grab":{"post":{"name":"grab","public":true}},
        "/named":{"post":{"name":"named","public":true}},"/status":{"post":{"name":"status","public":true}},
        "/sub":{"post":{"name":"sub","public":true}},"/digest":{"post":{"name":"digest","public":true}},
        "/cold":{"post":{"name":"cold","public":true}},"/prefs":{"post":{"name":"prefs","public":true}}}}})",
                                   module, &err))
      << err;
}

// A box invites through its service, with declared roles the host config allows, as the site when
// nobody is signed in (anonymous writes are budgeted, so each test makes few).
TEST_F(InvitesHostTest, ABoxInvitesWithinItsGrants) {
  start(kTenants);
  upload_club(*host);
  auto call = [&](const std::string& path) { return fetch(port, kSite, bhttp::verb::post, path).body; };
  // A role the box didn't declare is refused; one the host config doesn't allow can't even be
  // declared (refused at upload). A timer or anonymous request can't sign an invite with a
  // person's name.
  EXPECT_NE(call("/grab").find("isn't one this service's invites declare"), std::string::npos);
  std::string err;
  EXPECT_FALSE(host->boxes().upload(R"({"box":"grabby","domains":["kc.example.com"],
      "external":{"i":{"invites":{"roles":["owner"]}}}})", "(module (memory (export \"memory\") 1))", &err));
  EXPECT_NE(err.find("tenants.kc.invites.roles"), std::string::npos) << err;
  EXPECT_FALSE(host->boxes().upload(R"({"box":"rival","domains":["kc.example.com"],
      "external":{"m":{"mail":{"lists":{"digest":{"title":"Another digest"}}}}}})", "(module (memory (export \"memory\") 1))", &err));
  EXPECT_NE(err.find("one list, one title"), std::string::npos) << err;
  EXPECT_NE(call("/named").find("needs a signed-in person"), std::string::npos);
  auto r = call("/inv");
  EXPECT_NE(r.find(R"("accepted":true)"), std::string::npos) << r;
  EXPECT_EQ(mails_to("hal@example.com"), 0);  // after the commit, through the outbox
  EXPECT_EQ(host->outbox_tick(base::now_unix_ms()), 1);
  std::string mail = newest_mail("hal@example.com");
  EXPECT_NE(mail.find("kc.example.com invited you to kc.example.com"), std::string::npos) << mail;  // the site
  EXPECT_NE(mail.find("The daily digest"), std::string::npos);
  auto list = tool("invite_list", R"({"tenant":"kc","box":"club"})").at("value").at("invites").as_array();
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0].at("inviter").at("kind"), "site");
  EXPECT_NE(call("/status").find(R"("state":"open")"), std::string::npos);

}

// A box asks people to confirm a list, and sends the list's mail only to those who did.
TEST_F(InvitesHostTest, ABoxRunsAListOnlyWithConsent) {
  start(kTenants);
  upload_club(*host);
  auto call = [&](const std::string& path) { return fetch(port, kSite, bhttp::verb::post, path).body; };
  // Lists: no opt-in, no list mail; transactional needs a relationship.
  EXPECT_NE(call("/digest").find("isn't subscribed to digest"), std::string::npos);
  EXPECT_NE(call("/cold").find("transactional mail goes to an account"), std::string::npos);
  EXPECT_NE(call("/sub").find(R"("accepted":true)"), std::string::npos);
  EXPECT_NE(call("/sub").find("one is enough"), std::string::npos);  // the second within the request's
  host->outbox_tick(base::now_unix_ms());
  std::string confirm_mail = newest_mail("jo@example.com");
  std::string confirm = link(confirm_mail, "/goober/list/");
  ASSERT_FALSE(confirm.empty()) << confirm_mail;
  auto ask = get(confirm);
  EXPECT_NE(ask.body.find("Subscribe to The daily digest?"), std::string::npos) << ask.body;
  EXPECT_NE(call("/prefs").find(R"("digest":"none")"), std::string::npos);
  EXPECT_EQ(post_form(confirm, "").status, 200);
  EXPECT_NE(call("/prefs").find(R"("digest":"in")"), std::string::npos);
  auto sent = call("/digest");
  EXPECT_NE(sent.find(R"("category":"list")"), std::string::npos) << sent;
  host->outbox_tick(base::now_unix_ms());
  std::string digest = newest_mail("jo@example.com");
  EXPECT_NE(digest.find("List-Id: <digest.kc.kc.example.com>"), std::string::npos) << digest;
  EXPECT_NE(digest.find("because you subscribed to The daily digest"), std::string::npos);
  // Its one-click unsubscribe leaves the list, and nothing else.
  std::string unsub = link(digest, "/goober/mail/");
  EXPECT_NE(unsub.find("/list.digest"), std::string::npos) << unsub;
  EXPECT_NE(digest.find(unsub + ">"), std::string::npos);  // the same link in List-Unsubscribe
  auto left = fetch(port, kSite, bhttp::verb::post, unsub, {{"content-type", "application/x-www-form-urlencoded"}},
                    "List-Unsubscribe=One-Click");
  EXPECT_EQ(left.status, 200);
  EXPECT_NE(left.body.find("You've left The daily digest"), std::string::npos) << left.body;
  EXPECT_FALSE(host->mail_suppressed("kc", "jo@example.com"));
  EXPECT_NE(call("/digest").find("isn't subscribed"), std::string::npos);
  // An operator can't re-subscribe someone who left.
  auto record = tool("mail_consent_record", R"({"tenant":"kc","email":"jo@example.com","list":"digest",
      "statement":"Signed up at the 11/2 meetup"})");
  EXPECT_TRUE(record.at("isError").as_bool());
  auto lists = tool("mail_lists", R"({"tenant":"kc"})").at("value").at("lists").as_array();
  ASSERT_EQ(lists.size(), 1u);
  EXPECT_EQ(lists[0].at("box"), "club");
}

TEST_F(InvitesHostTest, AcceptingAnInviteJoinsItsListsWithTheInviteAsEvidence) {
  start(kTenants);
  std::string module = calling_module({{"inv", "club", "send", R"({"email":"lou@example.com","roles":["member"],"lists":["digest"]})"}});
  std::string err;
  ASSERT_TRUE(host->boxes().upload(R"({"box":"club","domains":["kc.example.com"],
      "external":{"mail":{"mail":{"lists":{"digest":{"title":"The daily digest","about":"One email a day."}}}},
                  "club":{"invites":{"roles":["member"],"lists":["digest"]}}},
      "routes":{"http":{"/inv":{"post":{"name":"inv","public":true}}}}})",
                                   module, &err))
      << err;
  fetch(port, kSite, bhttp::verb::post, "/inv");
  host->outbox_tick(base::now_unix_ms());
  std::string path = link(newest_mail("lou@example.com"), "/identity/invite/");
  // Signing in some other way shows the invite first; nothing is accepted without its page.
  std::string cookie = sign_in("lou@example.com");
  ASSERT_FALSE(cookie.empty());
  auto me = parse(fetch(port, kSite, bhttp::verb::get, "/identity/me", {{"cookie", cookie}}).body).as_object();
  EXPECT_TRUE(me.at("roles").as_array().empty());
  // Signed in as the invited address, Continue accepts at once.
  auto done = post_form(path, "", cookie);
  EXPECT_EQ(done.status, 303);
  me = parse(fetch(port, kSite, bhttp::verb::get, "/identity/me", {{"cookie", cookie}}).body).as_object();
  EXPECT_EQ(me.at("roles").as_array().size(), 1u);
  auto prefs = tool("mail_preferences", R"({"tenant":"kc","email":"lou@example.com"})").at("value").as_object();
  EXPECT_EQ(prefs.at("lists").at("digest").at("state"), "in");
  EXPECT_EQ(prefs.at("lists").at("digest").at("how"), "invite");
  EXPECT_EQ(prefs.at("relationship"), "account");
}
