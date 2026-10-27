// End to end: the OAuth endpoints, consent, bearer tokens on box routes, and agents
// (design/machine/009.OAuth.md; 999.Mobile.md § Signing in).

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/router/path.h"

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

Reply fetch(uint16_t port, bhttp::verb verb, const std::string& target,
            std::vector<std::pair<std::string, std::string>> headers = {}, const std::string& body = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "localhost:8080");
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

const char* kEchoWat = R"((module
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
  (func (export "goober_route_mine") (result i32) (call $echo))
  (func (export "goober_route_for_agents") (result i32) (call $echo))
))";

const std::string kVerifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
const std::string kChallenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
const std::string kApp = "com.mathgladiator.dumb:/oauth/callback";
const std::vector<std::pair<std::string, std::string>> kForm = {{"content-type", "application/x-www-form-urlencoded"}};

std::string q(const std::string& s) { return base::percent_encode(s); }

}  // namespace

class OAuthRoutesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "oauth_routes";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"localhost": {"tenant": "playground", "owner": "exec@example.com"}},
        "identity": {"clients": {"dumb-terminal": {"name": "dumb terminal", "acts-as": "person",
            "redirect-uris": ["com.mathgladiator.dumb:/oauth/callback", "http://127.0.0.1/callback"]}}},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
    ASSERT_TRUE(host->boxes().upload(R"({"box":"who","domains":["*"],"routes":{"http":{
        "/mine": {"get": {"name": "mine", "requirements": [["owner"]]}},
        "/for-agents": {"get": {"name": "for_agents", "requirements": [["owner"]], "agents": true}}}}})",
                                     kEchoWat, &err))
        << err;
    cookie = sign_in("exec%40example.com");
  }
  void TearDown() override { host.reset(); }

  std::string newest_code() {
    auto files = base::list_files(root / "storage/mail/outbox");
    std::sort(files.begin(), files.end());
    auto text = *base::read_file(root / "storage/mail/outbox" / files.back());
    std::smatch m;
    EXPECT_TRUE(std::regex_search(text, m, std::regex("is ([0-9]{6})")));
    return m[1];
  }
  std::string sign_in(const std::string& email) {
    fetch(port, bhttp::verb::post, "/identity/email/start", kForm, "email=" + email);
    auto v = fetch(port, bhttp::verb::post, "/identity/email/verify", kForm, "email=" + email + "&code=" + newest_code());
    EXPECT_EQ(v.status, 303);
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }
  std::string authorize_query(const std::string& redirect = kApp) {
    return "response_type=code&client_id=dumb-terminal&redirect_uri=" + q(redirect) + "&code_challenge=" + kChallenge +
           "&code_challenge_method=S256&state=xyz";
  }
  // Consents as the signed-in person and returns the code from the redirect.
  std::string consent(const std::string& redirect = kApp) {
    auto r = fetch(port, bhttp::verb::post, "/oauth/authorize",
                   {{"content-type", "application/x-www-form-urlencoded"}, {"cookie", cookie}, {"origin", "http://localhost:8080"}},
                   authorize_query(redirect) + "&decision=allow");
    EXPECT_EQ(r.status, 303) << r.body;
    std::string loc = r.headers["location"];
    EXPECT_TRUE(loc.starts_with(redirect + "?code=")) << loc;
    EXPECT_NE(loc.find("&state=xyz"), std::string::npos);
    EXPECT_NE(loc.find("&iss=http%3A%2F%2Flocalhost%3A8080"), std::string::npos);
    return router::parse_query(loc.substr(loc.find('?') + 1))["code"];
  }
  bj::object token(const std::string& form) {
    auto r = fetch(port, bhttp::verb::post, "/oauth/token", kForm, form);
    EXPECT_EQ(r.headers["cache-control"], "no-store");
    std::string err;
    auto v = json::parse(r.body, &err);
    auto o = v ? v->as_object() : bj::object{};
    o["_status"] = r.status;
    return o;
  }
  std::string str(const bj::object& o, const char* k) { return o.contains(k) ? std::string(o.at(k).as_string()) : ""; }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
  std::string cookie;
};

TEST_F(OAuthRoutesTest, TheTerminalSignsInAndCallsBoxRoutesAsThePerson) {
  auto meta = fetch(port, bhttp::verb::get, "/.well-known/oauth-authorization-server");
  ASSERT_EQ(meta.status, 200);
  EXPECT_NE(meta.body.find(R"("authorization_endpoint":"http://localhost:8080/oauth/authorize")"), std::string::npos);
  EXPECT_NE(meta.body.find(R"("code_challenge_methods_supported":["S256"])"), std::string::npos);

  // Signed out, authorize sends the browser to sign in and back.
  auto out = fetch(port, bhttp::verb::get, "/oauth/authorize?" + authorize_query());
  EXPECT_EQ(out.status, 303);
  EXPECT_TRUE(out.headers["location"].starts_with("/identity/sign-in?return=%2Foauth%2Fauthorize%3F"));

  // Signed in, the consent screen says who is asking and from where.
  auto page = fetch(port, bhttp::verb::get, "/oauth/authorize?" + authorize_query(), {{"cookie", cookie}});
  ASSERT_EQ(page.status, 200);
  EXPECT_NE(page.body.find("the app that opens com.mathgladiator.dumb: links"), std::string::npos);
  EXPECT_NE(page.body.find("as you"), std::string::npos);
  EXPECT_NE(page.body.find("<code>owner</code>"), std::string::npos);
  EXPECT_NE(page.headers["content-security-policy"].find("form-action 'self' com.mathgladiator.dumb:"), std::string::npos);
  EXPECT_EQ(page.headers["x-frame-options"], "DENY");

  // A forged consent never lands.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/oauth/authorize",
                  {{"content-type", "application/x-www-form-urlencoded"}, {"cookie", cookie}, {"origin", "https://evil.test"}},
                  authorize_query() + "&decision=allow")
                .status,
            403);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/oauth/authorize",
                  {{"content-type", "application/x-www-form-urlencoded"}, {"cookie", cookie}},
                  authorize_query() + "&decision=allow")
                .status,
            403);

  auto t = token("grant_type=authorization_code&client_id=dumb-terminal&code=" + consent() +
                 "&redirect_uri=" + q(kApp) + "&code_verifier=" + kVerifier);
  ASSERT_EQ(t["_status"], 200) << json::dump(t);
  EXPECT_EQ(t["token_type"], "Bearer");
  std::string access = str(t, "access_token");

  // The terminal is the person: it reaches the owner's route, and the box sees no agent.
  std::vector<std::pair<std::string, std::string>> api = {{"authorization", "Bearer " + access},
                                                          {"accept", "application/json"}};
  auto mine = fetch(port, bhttp::verb::get, "/mine", api);
  ASSERT_EQ(mine.status, 200) << mine.body;
  EXPECT_NE(mine.body.find(R"("email":"exec@example.com")"), std::string::npos);
  EXPECT_NE(mine.body.find(R"("agent":false)"), std::string::npos);
  EXPECT_EQ(mine.body.find("authorization"), std::string::npos);  // credentials never reach a box
  auto me = fetch(port, bhttp::verb::get, "/identity/me", api);
  EXPECT_NE(me.body.find(R"("client_id":"dumb-terminal")"), std::string::npos);

  // API callers get 401 with JSON, never a sign-in redirect.
  auto anon = fetch(port, bhttp::verb::get, "/mine", {{"accept", "application/json"}});
  EXPECT_EQ(anon.status, 401);
  EXPECT_EQ(anon.headers["www-authenticate"], "Bearer");
  EXPECT_NE(anon.body.find(R"("status":401)"), std::string::npos);
  auto bad = fetch(port, bhttp::verb::get, "/mine", {{"authorization", "Bearer goa_nope"}, {"accept", "text/html"}});
  EXPECT_EQ(bad.status, 401);
  EXPECT_EQ(bad.headers["www-authenticate"], "Bearer error=\"invalid_token\"");

  // Refresh rotates; the old refresh token then revokes the grant.
  auto t2 = token("grant_type=refresh_token&client_id=dumb-terminal&refresh_token=" + str(t, "refresh_token"));
  ASSERT_EQ(t2["_status"], 200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/mine", {{"authorization", "Bearer " + str(t2, "access_token")}}).status, 200);
  auto reuse = token("grant_type=refresh_token&client_id=dumb-terminal&refresh_token=" + str(t, "refresh_token"));
  EXPECT_EQ(reuse["_status"], 400);
  EXPECT_EQ(reuse["error"], "invalid_grant");
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/mine", {{"authorization", "Bearer " + str(t2, "access_token")}}).status, 401);

  // The desktop app's loopback redirect may use any port; sign-out revokes.
  auto t3 = token("grant_type=authorization_code&client_id=dumb-terminal&code=" + consent("http://127.0.0.1:43123/callback") +
                  "&redirect_uri=" + q("http://127.0.0.1:43123/callback") + "&code_verifier=" + kVerifier);
  ASSERT_EQ(t3["_status"], 200) << json::dump(t3);
  auto account = fetch(port, bhttp::verb::get, "/identity/account", {{"cookie", cookie}});
  EXPECT_NE(account.body.find("dumb terminal"), std::string::npos);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/oauth/revoke", kForm,
                  "client_id=dumb-terminal&token=" + str(t3, "refresh_token")).status,
            200);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/mine", {{"authorization", "Bearer " + str(t3, "access_token")}}).status, 401);
}

TEST_F(OAuthRoutesTest, BrokenRequestsAreRefusedWithoutRedirecting) {
  auto unknown = fetch(port, bhttp::verb::get, "/oauth/authorize?response_type=code&client_id=nope&redirect_uri=" + q(kApp));
  EXPECT_EQ(unknown.status, 400);
  auto elsewhere = fetch(port, bhttp::verb::get, "/oauth/authorize?" + authorize_query("https://evil.test/cb"));
  EXPECT_EQ(elsewhere.status, 400);
  EXPECT_TRUE(elsewhere.headers["location"].empty());
  auto plain = fetch(port, bhttp::verb::get,
                     "/oauth/authorize?response_type=code&client_id=dumb-terminal&redirect_uri=" + q(kApp) + "&state=s");
  EXPECT_EQ(plain.status, 303);
  EXPECT_NE(plain.headers["location"].find("error=invalid_request"), std::string::npos);
  auto t = token("grant_type=password&client_id=dumb-terminal");
  EXPECT_EQ(t["error"], "unsupported_grant_type");
  auto who = token("grant_type=authorization_code&client_id=ghost&code=x");
  EXPECT_EQ(who["_status"], 401);
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/.well-known/other").status, 404);
}

TEST_F(OAuthRoutesTest, AgentsUseTheDeviceFlowAndReachOnlyRoutesThatAllowThem) {
  auto reg = fetch(port, bhttp::verb::post, "/oauth/register", {{"content-type", "application/json"}},
                   R"({"client_name":"Grok","redirect_uris":[]})");
  ASSERT_EQ(reg.status, 201) << reg.body;
  std::string err;
  std::string client_id(json::parse(reg.body, &err)->at("client_id").as_string());

  auto start = fetch(port, bhttp::verb::post, "/oauth/device", kForm, "client_id=" + client_id);
  ASSERT_EQ(start.status, 200) << start.body;
  auto d = json::parse(start.body, &err)->as_object();
  EXPECT_EQ(d.at("verification_uri"), "http://localhost:8080/oauth/device/verify");
  EXPECT_FALSE(d.contains("verification_uri_complete"));
  std::string user_code(d.at("user_code").as_string());
  std::string device_code(d.at("device_code").as_string());
  // The terminal's client can't use the device flow.
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/oauth/device", kForm, "client_id=dumb-terminal").status, 400);

  std::vector<std::pair<std::string, std::string>> same = {
      {"content-type", "application/x-www-form-urlencoded"}, {"cookie", cookie}, {"origin", "http://localhost:8080"}};
  auto check = fetch(port, bhttp::verb::post, "/oauth/device/verify", same, "step=check&user_code=" + user_code);
  ASSERT_EQ(check.status, 200);
  EXPECT_NE(check.body.find("Grok"), std::string::npos);
  EXPECT_NE(check.body.find("unverified"), std::string::npos);
  auto allow = fetch(port, bhttp::verb::post, "/oauth/device/verify", same,
                     "step=decide&decision=allow&user_code=" + user_code);
  EXPECT_EQ(allow.status, 200);

  auto t = token("grant_type=" + q("urn:ietf:params:oauth:grant-type:device_code") + "&client_id=" + client_id +
                 "&device_code=" + device_code);
  ASSERT_EQ(t["_status"], 200) << json::dump(t);
  std::vector<std::pair<std::string, std::string>> api = {{"authorization", "Bearer " + str(t, "access_token")},
                                                          {"accept", "application/json"}};
  // The agent holds the owner's roles, yet only routes that opted in let it through.
  EXPECT_EQ(fetch(port, bhttp::verb::get, "/mine", api).status, 403);
  auto ok = fetch(port, bhttp::verb::get, "/for-agents", api);
  ASSERT_EQ(ok.status, 200) << ok.body;
  EXPECT_NE(ok.body.find(R"("agent":true)"), std::string::npos);
  EXPECT_NE(ok.body.find(R"("client_id":")" + client_id + "\""), std::string::npos);
}
