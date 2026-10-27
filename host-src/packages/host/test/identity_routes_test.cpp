#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

#include "goober/base/encoding.h"
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

struct Reply {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;  // lowercase names; the last value wins
};

Reply fetch(uint16_t port, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "localhost");
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

const char* kWhoWat = R"((module
  (import "goober" "goober_request_size" (func $size (result i32)))
  (import "goober" "goober_request_read" (func $read (param i32 i32) (result i32)))
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 2)
  (data (i32.const 0) "application/json")
  (func $echo (result i32)
    (local $n i32)
    (local.set $n (call $read (i32.const 1024) (call $size)))
    (drop (call $respond (i32.const 200) (i32.const 0) (i32.const 16) (i32.const 1024) (local.get $n)))
    (i32.const 0))
  (func (export "goober_route_open") (result i32) (call $echo))
  (func (export "goober_route_owners") (result i32) (call $echo))
  (func (export "goober_route_staff") (result i32) (call $echo))
))";

class IdentityRoutesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "identity_routes";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "play.example",
        "domains": {"play.example": {"tenant": "playground", "owner": "boss@example.com"},
                    "second.example": {"tenant": "second"}},
        "operations": {"token": "test-token-0123456789"},
        "identity": {"operators": {"op@example.com": ["host:owner"]}},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    std::string manifest = R"({"box":"who","domains":["*"],"routes":{"http":{
        "/open": {"get": {"name": "open", "public": true}},
        "/owners": {"get": {"name": "owners", "requirements": [["owner"]]}},
        "/staff": {"get": {"name": "staff", "requirements": [["host:owner"]]}}}}})";
    auto up = host->boxes().upload(manifest, kWhoWat, &err);
    ASSERT_TRUE(up) << err;
  }
  void TearDown() override { host.reset(); }

  // The code in the newest mail in the dev mailer's outbox.
  std::string newest_code() {
    auto files = base::list_files(root / "storage/mail/outbox");
    EXPECT_FALSE(files.empty());
    std::sort(files.begin(), files.end());
    auto text = *base::read_file(root / "storage/mail/outbox" / files.back());
    std::smatch m;
    EXPECT_TRUE(std::regex_search(text, m, std::regex("is ([0-9]{6})")));
    return m[1];
  }

  // Signs in through the forms and returns the cookie header to send back.
  std::string sign_in(const std::string& email, bool as_host = false) {
    std::string as = as_host ? "&as=host" : "";
    auto start = fetch(port, bhttp::verb::post, "/identity/email/start",
                       {{"content-type", "application/x-www-form-urlencoded"}},
                       "email=" + email + "&return=%2Fowners" + as);
    EXPECT_EQ(start.status, 200);
    auto verify = fetch(port, bhttp::verb::post, "/identity/email/verify",
                        {{"content-type", "application/x-www-form-urlencoded"}},
                        "email=" + email + "&code=" + newest_code() + "&return=%2Fowners" + as);
    EXPECT_EQ(verify.status, 303) << verify.body;
    EXPECT_EQ(verify.headers["location"], "/owners");
    std::string cookie = verify.headers["set-cookie"];
    EXPECT_NE(cookie.find("HttpOnly"), std::string::npos);
    EXPECT_EQ(cookie.find("Secure"), std::string::npos);  // local mode
    return cookie.substr(0, cookie.find(';'));
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

}  // namespace

TEST_F(IdentityRoutesTest, SignInPageIsHostOwnedAndUnframeable) {
  auto r = fetch(port, bhttp::verb::get, "/identity/sign-in?return=%2Fowners");
  EXPECT_EQ(r.status, 200);
  EXPECT_NE(r.body.find("Email me a code"), std::string::npos);
  EXPECT_NE(r.body.find("Local mode"), std::string::npos);
  EXPECT_EQ(r.headers["x-frame-options"], "DENY");
  // The customer page says nothing of host operators; they have their own unlinked page.
  EXPECT_EQ(r.body.find("operator"), std::string::npos);
  auto h = fetch(port, bhttp::verb::get, "/identity/sign-in/as-host");
  EXPECT_EQ(h.status, 200);
  EXPECT_NE(h.body.find("Host sign-in"), std::string::npos);
  EXPECT_NE(h.body.find("name=\"as\" value=\"host\""), std::string::npos);
  EXPECT_EQ(h.headers["x-frame-options"], "DENY");
  // The browser must send a real Origin with the form, so it can pass same_origin.
  EXPECT_EQ(h.headers["referrer-policy"], "same-origin");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/identity/nope").status, 404);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/identity/me").status, 401);
  // A form posted from another origin is refused.
  auto cross = fetch(port, bhttp::verb::post, "/identity/email/start", {{"origin", "https://evil.example"}},
                     "email=boss@example.com");
  EXPECT_EQ(cross.status, 403);
  // An opaque origin (a sandboxed frame, a data: page) is refused too.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/identity/email/start", {{"origin", "null"}}, "email=boss@example.com")
                .status,
            403);
  // A same-origin form gets through.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/identity/email/start",
                  {{"origin", "http://localhost"}}, "email=boss@example.com")
                .status,
            200);
}

TEST_F(IdentityRoutesTest, RequirementsGateRoutesAndTheFirstOwnerIsBootstrapped) {
  // Signed out: browsers are sent to sign in, everyone else gets a 401.
  auto r = fetch(port, bhttp::verb::get, "/owners?x=1", {{"accept", "text/html"}});
  EXPECT_EQ(r.status, 303);
  EXPECT_EQ(r.headers["location"], "/identity/sign-in?return=%2Fowners%3Fx%3D1");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/owners").status, 401);
  r = fetch(port, bhttp::verb::get, "/open");
  ASSERT_EQ(r.status, 200);
  std::string err;
  EXPECT_TRUE(json::parse(r.body, &err)->at("user").is_null());

  std::string boss = sign_in("boss%40example.com");
  r = fetch(port, bhttp::verb::get, "/owners", {{"cookie", boss}});
  ASSERT_EQ(r.status, 200) << r.body;
  auto doc = *json::parse(r.body, &err);
  EXPECT_EQ(doc.at("user").at("email"), "boss@example.com");
  EXPECT_EQ(doc.at("user").at("roles").at(0), "owner");
  EXPECT_EQ(doc.at("user").at("host"), false);
  EXPECT_EQ(doc.at("headers").as_object().count("cookie"), 0u);
  auto me = fetch(port, bhttp::verb::get, "/identity/me", {{"cookie", boss}});
  EXPECT_EQ(me.status, 200);
  EXPECT_NE(me.body.find("\"realm\":\"playground\""), std::string::npos);

  // Anyone may sign up, but only the configured owner gets "owner".
  std::string guest = sign_in("guest%40example.com");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/owners", {{"cookie", guest}}).status, 403);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/staff", {{"cookie", boss}}).status, 403);

  // Sign out ends the session.
  // A cookie-bearing POST that doesn't say where it came from is refused (CSRF).
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/identity/sign-out", {{"cookie", boss}}, "").status, 403);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/identity/sign-out", {{"cookie", boss}, {"origin", "https://localhost"}}, "").status,
            403);  // the scheme counts
  auto out = fetch(port, bhttp::verb::post, "/identity/sign-out", {{"cookie", boss}, {"origin", "http://localhost"}}, "");
  EXPECT_EQ(out.status, 303);
  EXPECT_NE(out.headers["set-cookie"].find("Max-Age=0"), std::string::npos);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/owners", {{"cookie", boss}}).status, 401);
}

TEST_F(IdentityRoutesTest, HostOperatorsSatisfyOnlyHostRoles) {
  std::string op = sign_in("op%40example.com", true);
  EXPECT_EQ(op.substr(0, 17), "goober-session=h.");
  auto r = fetch(port, bhttp::verb::get, "/staff", {{"cookie", op}});
  ASSERT_EQ(r.status, 200) << r.body;
  std::string err;
  EXPECT_EQ(json::parse(r.body, &err)->at("user").at("host"), true);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/owners", {{"cookie", op}}).status, 403);

  // An address that isn't an operator never gets a host code.
  auto before = base::list_files(root / "storage/mail/outbox").size();
  fetch(port, bhttp::verb::post, "/identity/email/start", {}, "email=boss%40example.com&as=host");
  EXPECT_EQ(base::list_files(root / "storage/mail/outbox").size(), before);
}

TEST_F(IdentityRoutesTest, OperatorToolsGrantRolesAndLocalAliasesReachOtherDomains) {
  auto tool = [&](const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                   {{"authorization", "Bearer test-token-0123456789"}}, json::dump(req));
    std::string err;
    return *json::parse(r.body, &err);
  };
  auto g = tool("role_grant", {{"tenant", "playground"}, {"email", "guest@example.com"}, {"role", "owner"}});
  EXPECT_EQ(g.at("result").at("isError"), false) << json::dump(g);
  EXPECT_EQ(tool("role_grant", {{"tenant", "nope"}, {"email", "a@b.c"}, {"role", "x"}}).at("result").at("isError"),
            true);
  std::string guest = sign_in("guest%40example.com");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/owners", {{"cookie", guest}}).status, 200);
  auto realm = tool("realm_get", {{"tenant", "playground"}});
  EXPECT_NE(json::dump(realm).find("guest@example.com"), std::string::npos);
  EXPECT_TRUE(fs::exists(root / "storage/identity/playground.db"));

  // second.example.localhost reaches second.example, a different realm: the cookie means nothing there.
  auto r = fetch(port, bhttp::verb::get, "/owners", {{"host", "second.example.localhost"}, {"cookie", guest}});
  EXPECT_EQ(r.status, 401);
  r = fetch(port, bhttp::verb::get, "/open", {{"host", "second.example.localhost"}});
  std::string err;
  EXPECT_EQ(json::parse(r.body, &err)->at("tenant"), "second");
}
