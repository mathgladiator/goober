// Operator OAuth (design/machine/023.OperatorOAuth.md): an MCP client that only speaks OAuth
// (Grok's connector) finds the operator issuer from /goober-operations-mcp's 401, registers,
// sends a host operator through sign-in and consent, and calls the operator tools.

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/time.h"
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

bj::object parse(const std::string& s) {
  std::string err;
  auto v = json::parse(s, &err);
  return v && v->is_object() ? v->as_object() : bj::object{};
}

const std::string kVerifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
const std::string kChallenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
const std::string kRedirect = "https://grok.example/callback";
const std::vector<std::pair<std::string, std::string>> kForm = {{"content-type", "application/x-www-form-urlencoded"}};
const char* kListTools = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";

}  // namespace

class OperatorOAuthTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "operator_oauth";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({
        "listen": {"port": 0}, "storage": "storage", "platform-domain": "localhost",
        "domains": {"localhost": {"tenant": "playground"}},
        "identity": {"operators": {"boss@example.com": ["host:owner"]}},
        "threads": {"network": 1, "boxes": 2}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  std::string newest_code() {
    host->drain_mail();
    auto files = base::list_files(root / "storage/mail/outbox");
    std::sort(files.begin(), files.end());
    auto text = *base::read_file(root / "storage/mail/outbox" / files.back());
    std::smatch m;
    EXPECT_TRUE(std::regex_search(text, m, std::regex("is ([0-9]{6})")));
    return m[1];
  }
  // Signs in through the forms, as a host operator or as a person of the site.
  std::string sign_in(const std::string& email, bool as_host) {
    std::string as = as_host ? "&as=host" : "";
    fetch(port, bhttp::verb::post, "/identity/email/start", kForm, "email=" + email + as);
    auto v = fetch(port, bhttp::verb::post, "/identity/email/verify", kForm,
                   "email=" + email + as + "&code=" + newest_code());
    EXPECT_EQ(v.status, 303) << v.body;
    return v.headers["set-cookie"].substr(0, v.headers["set-cookie"].find(';'));
  }
  std::string authorize_query(const std::string& client_id) {
    return "response_type=code&client_id=" + client_id + "&redirect_uri=" + base::percent_encode(kRedirect) +
           "&code_challenge=" + kChallenge + "&code_challenge_method=S256&state=s1";
  }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(OperatorOAuthTest, AnOAuthOnlyClientReachesTheOperatorTools) {
  // 1. The 401 says where to learn how to sign in.
  auto denied = fetch(port, bhttp::verb::post, "/goober-operations-mcp", {{"content-type", "application/json"}}, kListTools);
  ASSERT_EQ(denied.status, 401);
  const std::string resource_meta = "/.well-known/oauth-protected-resource/goober-operations-mcp";
  EXPECT_NE(denied.headers["www-authenticate"].find("resource_metadata=\"http://localhost:8080" + resource_meta + "\""),
            std::string::npos)
      << denied.headers["www-authenticate"];

  // 2. The resource names the operator issuer, and the issuer's metadata is where RFC 8414 says.
  auto resource = parse(fetch(port, bhttp::verb::get, resource_meta).body);
  EXPECT_EQ(resource["resource"], "http://localhost:8080/goober-operations-mcp");
  EXPECT_EQ(resource["authorization_servers"].as_array().at(0), "http://localhost:8080/oauth/operator");
  auto meta = parse(fetch(port, bhttp::verb::get, "/.well-known/oauth-authorization-server/oauth/operator").body);
  EXPECT_EQ(meta["issuer"], "http://localhost:8080/oauth/operator");
  EXPECT_EQ(meta["authorization_endpoint"], "http://localhost:8080/oauth/operator/authorize");
  EXPECT_EQ(meta["token_endpoint"], "http://localhost:8080/oauth/operator/token");
  EXPECT_EQ(parse(fetch(port, bhttp::verb::get, "/oauth/operator/.well-known/oauth-authorization-server").body)["issuer"],
            "http://localhost:8080/oauth/operator");
  // The site's own issuer is unchanged.
  EXPECT_EQ(parse(fetch(port, bhttp::verb::get, "/.well-known/oauth-authorization-server").body)["issuer"],
            "http://localhost:8080");

  // 3. The client registers itself with the operator issuer.
  auto reg = fetch(port, bhttp::verb::post, "/oauth/operator/register", {{"content-type", "application/json"}},
                   R"({"client_name":"Grok","redirect_uris":["https://grok.example/callback"]})");
  ASSERT_EQ(reg.status, 201) << reg.body;
  std::string client_id(parse(reg.body)["client_id"].as_string());

  // 4. Signed out, authorize sends the browser to the host operators' sign-in.
  auto out = fetch(port, bhttp::verb::get, "/oauth/operator/authorize?" + authorize_query(client_id));
  ASSERT_EQ(out.status, 303);
  EXPECT_TRUE(out.headers["location"].starts_with("/identity/sign-in/as-host?return=%2Foauth%2Foperator%2Fauthorize"))
      << out.headers["location"];
  // A person of the site is sent there too: their session isn't an operator's.
  std::string person = sign_in("ann%40example.com", false);
  auto as_person = fetch(port, bhttp::verb::get, "/oauth/operator/authorize?" + authorize_query(client_id), {{"cookie", person}});
  EXPECT_EQ(as_person.status, 303);

  // 5. The operator signs in and sees what they are handing over.
  std::string boss = sign_in("boss%40example.com", true);
  auto consent = fetch(port, bhttp::verb::get, "/oauth/operator/authorize?" + authorize_query(client_id), {{"cookie", boss}});
  ASSERT_EQ(consent.status, 200) << consent.body;
  EXPECT_NE(consent.body.find("as a host operator"), std::string::npos);
  EXPECT_NE(consent.body.find("registered itself"), std::string::npos);
  EXPECT_NE(consent.body.find("action=\"/oauth/operator/authorize\""), std::string::npos);
  auto allowed = fetch(port, bhttp::verb::post, "/oauth/operator/authorize",
                       {{"content-type", "application/x-www-form-urlencoded"}, {"cookie", boss}, {"origin", "http://localhost:8080"}},
                       authorize_query(client_id) + "&decision=allow");
  ASSERT_EQ(allowed.status, 303) << allowed.body;
  std::string loc = allowed.headers["location"];
  ASSERT_TRUE(loc.starts_with(kRedirect + "?code=")) << loc;
  EXPECT_NE(loc.find("&iss=http%3A%2F%2Flocalhost%3A8080%2Foauth%2Foperator"), std::string::npos) << loc;
  std::string code = router::parse_query(loc.substr(loc.find('?') + 1))["code"];

  // 6. The code becomes tokens, and the access token opens the operator tools.
  auto tok = parse(fetch(port, bhttp::verb::post, "/oauth/operator/token", kForm,
                         "grant_type=authorization_code&client_id=" + client_id + "&code=" + code +
                             "&redirect_uri=" + base::percent_encode(kRedirect) + "&code_verifier=" + kVerifier)
                       .body);
  ASSERT_TRUE(tok.contains("access_token")) << json::dump(tok);
  std::string access(tok["access_token"].as_string());
  auto tools = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                     {{"content-type", "application/json"}, {"authorization", "Bearer " + access}}, kListTools);
  ASSERT_EQ(tools.status, 200) << tools.body;
  EXPECT_GT(parse(tools.body)["result"].at("tools").as_array().size(), 60u);

  // The access log says which tool the agent called, as whom.
  auto log = base::read_file(root / "storage/logs/access.log").value_or("");
  EXPECT_NE(log.find("\"mcp_method\":\"tools/list\""), std::string::npos);
  EXPECT_NE(log.find("\"operator\":\"oauth:boss@example.com:"), std::string::npos);

  // The operator's token is only for operating: the site's /mcp doesn't take it.
  auto site = fetch(port, bhttp::verb::post, "/mcp", {{"content-type", "application/json"}, {"authorization", "Bearer " + access}},
                    kListTools);
  EXPECT_EQ(site.status, 401);

  // 7. Refresh works, and revoking ends access.
  auto refreshed = parse(fetch(port, bhttp::verb::post, "/oauth/operator/token", kForm,
                               "grant_type=refresh_token&client_id=" + client_id + "&refresh_token=" +
                                   std::string(tok["refresh_token"].as_string()))
                             .body);
  ASSERT_TRUE(refreshed.contains("access_token")) << json::dump(refreshed);
  std::string fresh(refreshed["access_token"].as_string());
  // goober-host operator revoke: every login and grant the operator holds ends at once.
  auto again = parse(fetch(port, bhttp::verb::post, "/oauth/operator/token", kForm,
                           "grant_type=refresh_token&client_id=" + client_id + "&refresh_token=" +
                               std::string(refreshed["refresh_token"].as_string()))
                         .body);
  ASSERT_TRUE(again.contains("access_token")) << json::dump(again);
  std::string live(again["access_token"].as_string());
  EXPECT_GT(host->identity().revoke_sessions(identity::kHostRealm, "boss@example.com", "test", base::now_unix_ms()), 0);
  EXPECT_EQ(fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                  {{"content-type", "application/json"}, {"authorization", "Bearer " + live}}, kListTools)
                .status,
            401);
  fetch(port, bhttp::verb::post, "/oauth/operator/revoke", kForm, "client_id=" + client_id + "&token=" + fresh);
  auto after = fetch(port, bhttp::verb::post, "/goober-operations-mcp",
                     {{"content-type", "application/json"}, {"authorization", "Bearer " + fresh}}, kListTools);
  EXPECT_EQ(after.status, 401);
}

TEST_F(OperatorOAuthTest, OnlyOperatorsCanSignInAndTheRealmsStayApart) {
  // An address not in identity.operators gets no code from the host sign-in.
  host->drain_mail();
  auto before = base::list_files(root / "storage/mail/outbox").size();
  fetch(port, bhttp::verb::post, "/identity/email/start", kForm, "email=mallory%40example.com&as=host");
  host->drain_mail();
  EXPECT_EQ(base::list_files(root / "storage/mail/outbox").size(), before);

  // A client registered with the site's issuer is unknown to the operator issuer.
  auto reg = fetch(port, bhttp::verb::post, "/oauth/register", {{"content-type", "application/json"}},
                   R"({"client_name":"Site agent","redirect_uris":["https://grok.example/callback"]})");
  ASSERT_EQ(reg.status, 201);
  std::string site_client(parse(reg.body)["client_id"].as_string());
  std::string boss = sign_in("boss%40example.com", true);
  auto r = fetch(port, bhttp::verb::get, "/oauth/operator/authorize?" + authorize_query(site_client), {{"cookie", boss}});
  EXPECT_EQ(r.status, 400);
  EXPECT_NE(r.body.find("registered here"), std::string::npos) << r.body;
}
