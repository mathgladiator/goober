// OAuth 2.1 in the identity service: clients, PKCE codes, rotating refresh tokens,
// revocation by grant, and the device flow (design/machine/009.OAuth.md).

#include <gtest/gtest.h>

#include <regex>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/identity/identity.h"

using namespace goober;
using namespace goober::identity;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kNow = 1'800'000'000'000;
constexpr int64_t kDay = 24LL * 3600 * 1000;
const std::string kVerifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
const std::string kChallenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
const std::string kAppRedirect = "com.mathgladiator.dumb:/oauth/callback";

class OAuthTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = fs::path(GOOBER_TEST_TMP) / "oauth";
    fs::remove_all(dir);
    id = std::make_unique<Identity>(dir, [this](const Mail& m) { mail.push_back(m); });
    Client terminal{"dumb-terminal", "dumb terminal", {kAppRedirect, "http://127.0.0.1/callback"}, true, "config", 0};
    std::string why;
    ASSERT_TRUE(id->put_client("playground", terminal, kNow, &why)) << why;
    user_id = sign_in("exec@example.com");
  }
  int64_t sign_in(const std::string& email) {
    id->start_email("playground", email, "localhost", true, kNow);
    std::smatch m;
    EXPECT_TRUE(std::regex_search(mail.back().text, m, std::regex("is ([0-9]{6})")));
    std::string why;
    auto in = id->verify_email("playground", email, std::string(m[1]), true, kNow, kDay, &why);
    EXPECT_TRUE(in) << why;
    return in ? in->user.user_id : 0;
  }
  std::optional<Tokens> code_flow(int64_t at = kNow) {
    std::string code = id->issue_code("playground", "dumb-terminal", user_id, kAppRedirect, kChallenge, at);
    OAuthError e;
    auto t = id->exchange_code("playground", "dumb-terminal", code, kAppRedirect, kVerifier, at + 1000, 30 * kDay, &e);
    EXPECT_TRUE(t) << e.code << ": " << e.description;
    return t;
  }
  fs::path dir;
  std::vector<Mail> mail;
  std::unique_ptr<Identity> id;
  int64_t user_id = 0;
};
}  // namespace

TEST(OAuthRedirects, LoopbackPortsMayDifferAndNothingElse) {
  Client c{"x", "x", {"http://127.0.0.1/callback", "com.example.app:/cb", "https://app.example.com/cb"}, false, "", 0};
  EXPECT_TRUE(redirect_allowed(c, "http://127.0.0.1/callback"));
  EXPECT_TRUE(redirect_allowed(c, "http://127.0.0.1:53817/callback"));
  EXPECT_TRUE(redirect_allowed(c, "com.example.app:/cb"));
  EXPECT_TRUE(redirect_allowed(c, "https://app.example.com/cb"));
  EXPECT_FALSE(redirect_allowed(c, "http://localhost:53817/callback"));
  EXPECT_FALSE(redirect_allowed(c, "http://127.0.0.1:53817/other"));
  EXPECT_FALSE(redirect_allowed(c, "http://127.0.0.1:53817/callback?x=1"));
  EXPECT_FALSE(redirect_allowed(c, "http://127.0.0.1:99999/callback"));
  EXPECT_FALSE(redirect_allowed(c, "http://127.0.0.1.evil.test/callback"));
  EXPECT_FALSE(redirect_allowed(c, "https://app.example.com:444/cb"));
  EXPECT_FALSE(redirect_allowed(c, "com.example.app:/cb/../x"));

  EXPECT_TRUE(is_valid_redirect_uri("com.mathgladiator.dumb:/oauth/callback"));
  EXPECT_TRUE(is_valid_redirect_uri("http://[::1]/cb"));
  EXPECT_FALSE(is_valid_redirect_uri("http://example.com/cb"));
  EXPECT_TRUE(is_valid_redirect_uri("http://localhost:33418/callback"));  // MCP clients; exact match only
  EXPECT_FALSE(is_valid_redirect_uri("http://localhost.evil.test/cb"));
  EXPECT_FALSE(is_valid_redirect_uri("http://localhost:/cb"));
  Client mcp{"m", "m", {"http://localhost:33418/callback"}, false, "", 0};
  EXPECT_TRUE(redirect_allowed(mcp, "http://localhost:33418/callback"));
  EXPECT_FALSE(redirect_allowed(mcp, "http://localhost:33419/callback"));
  EXPECT_FALSE(is_valid_redirect_uri("javascript:alert(1)"));
  EXPECT_TRUE(is_valid_redirect_uri("cursor://anysphere.cursor-mcp/oauth/callback"));  // one word, as Cursor sends
  EXPECT_FALSE(is_valid_redirect_uri("data:text/html,hi"));
  EXPECT_FALSE(is_valid_redirect_uri("file:///etc/passwd"));
  EXPECT_FALSE(is_valid_redirect_uri("Cursor://x"));
  EXPECT_FALSE(is_valid_redirect_uri("https://example.com/cb#frag"));
  EXPECT_FALSE(is_valid_redirect_uri("https:///cb"));
}

TEST_F(OAuthTest, CodeFlowIssuesPersonTokens) {
  auto t = code_flow();
  ASSERT_TRUE(t);
  EXPECT_EQ(t->expires_in_s, 3600);
  EXPECT_TRUE(t->access_token.starts_with("goa_"));
  auto b = id->bearer("playground", t->access_token, kNow + 2000);
  ASSERT_TRUE(b);
  EXPECT_EQ(b->user.user_id, user_id);
  EXPECT_EQ(b->client_id, "dumb-terminal");
  EXPECT_FALSE(b->agent);  // a person client's tokens act as the person
  // Tokens stay in their realm, and access tokens expire.
  EXPECT_FALSE(id->bearer("second", t->access_token, kNow + 2000));
  EXPECT_FALSE(id->bearer("playground", t->access_token, kNow + 3600 * 1000 + 1001));
  // A refresh token is not an access token, and a session token isn't either.
  EXPECT_FALSE(id->bearer("playground", t->refresh_token, kNow + 2000));
}

TEST_F(OAuthTest, CodesCheckEverythingAndWorkOnce) {
  auto refuse = [&](const std::string& client, const std::string& redirect, const std::string& verifier,
                    int64_t at, const char* want) {
    std::string code = id->issue_code("playground", "dumb-terminal", user_id, kAppRedirect, kChallenge, kNow);
    OAuthError e;
    EXPECT_FALSE(id->exchange_code("playground", client, code, redirect, verifier, at, kDay, &e));
    EXPECT_EQ(e.code, want) << e.description;
  };
  refuse("dumb-terminal", kAppRedirect, kVerifier + "x", kNow, "invalid_grant");
  refuse("dumb-terminal", kAppRedirect, "short", kNow, "invalid_grant");
  refuse("other", kAppRedirect, kVerifier, kNow, "invalid_grant");
  refuse("dumb-terminal", "http://127.0.0.1:5000/callback", kVerifier, kNow, "invalid_grant");
  refuse("dumb-terminal", kAppRedirect, kVerifier, kNow + kAuthorizationCodeTtlMs, "invalid_grant");

  // A replayed code revokes what the first exchange bought.
  std::string code = id->issue_code("playground", "dumb-terminal", user_id, kAppRedirect, kChallenge, kNow);
  OAuthError e;
  auto t = id->exchange_code("playground", "dumb-terminal", code, kAppRedirect, kVerifier, kNow, kDay, &e);
  ASSERT_TRUE(t);
  EXPECT_FALSE(id->exchange_code("playground", "dumb-terminal", code, kAppRedirect, kVerifier, kNow, kDay, &e));
  EXPECT_FALSE(id->bearer("playground", t->access_token, kNow + 1));
}

TEST_F(OAuthTest, RefreshRotatesAndReuseRevokesTheGrant) {
  auto t1 = code_flow();
  ASSERT_TRUE(t1);
  OAuthError e;
  auto t2 = id->refresh("playground", "dumb-terminal", t1->refresh_token, kNow + 5000, 30 * kDay, &e);
  ASSERT_TRUE(t2) << e.description;
  EXPECT_NE(t2->refresh_token, t1->refresh_token);
  // A second session sharing the grant keeps its access token until it runs out.
  EXPECT_TRUE(id->bearer("playground", t1->access_token, kNow + 6000));
  EXPECT_TRUE(id->bearer("playground", t2->access_token, kNow + 6000));
  // The same refresh token again within the grace is a client racing itself: it gets tokens.
  auto raced = id->refresh("playground", "dumb-terminal", t1->refresh_token, kNow + 5000 + kRefreshReuseGraceMs, 30 * kDay, &e);
  ASSERT_TRUE(raced) << e.description;
  EXPECT_TRUE(id->bearer("playground", t2->access_token, kNow + 6000 + kRefreshReuseGraceMs));
  EXPECT_TRUE(id->bearer("playground", raced->access_token, kNow + 6000 + kRefreshReuseGraceMs));
  // After it, a replay is two parties holding one grant: both lose it.
  int64_t late = kNow + 5001 + kRefreshReuseGraceMs;
  EXPECT_FALSE(id->refresh("playground", "dumb-terminal", t1->refresh_token, late, 30 * kDay, &e));
  EXPECT_EQ(e.code, "invalid_grant");
  EXPECT_FALSE(id->bearer("playground", t2->access_token, late + 1));
  EXPECT_FALSE(id->bearer("playground", raced->access_token, late + 1));
  EXPECT_FALSE(id->refresh("playground", "dumb-terminal", t2->refresh_token, late + 2, 30 * kDay, &e));
  // Another client can't use someone else's refresh token.
  auto t3 = code_flow();
  EXPECT_FALSE(id->refresh("playground", "other", t3->refresh_token, kNow + 5000, kDay, &e));
}

TEST_F(OAuthTest, RevokingEndsTheWholeGrantAndListsIt) {
  auto t = code_flow();
  auto grants = id->grants("playground", user_id, kNow + 2000);
  ASSERT_EQ(grants.size(), 1u);
  EXPECT_EQ(grants[0].at("client_id"), "dumb-terminal");
  EXPECT_EQ(grants[0].at("acts_as"), "person");
  // Another client's revoke is ignored (RFC 7009).
  id->revoke_token("playground", "other", t->refresh_token, kNow + 3000);
  EXPECT_TRUE(id->bearer("playground", t->access_token, kNow + 3000));
  id->revoke_token("playground", "dumb-terminal", t->refresh_token, kNow + 3000);
  EXPECT_FALSE(id->bearer("playground", t->access_token, kNow + 4000));
  EXPECT_EQ(id->grants("playground", user_id, kNow + 4000).size(), 0u);

  // The person revokes from their account page.
  auto t2 = code_flow();
  std::string family(id->grants("playground", user_id, kNow + 2000)[0].at("grant").as_string());
  EXPECT_FALSE(id->revoke_grant("playground", user_id + 1, family, kNow + 3000));  // not theirs
  EXPECT_TRUE(id->revoke_grant("playground", user_id, family, kNow + 3000));
  EXPECT_FALSE(id->bearer("playground", t2->access_token, kNow + 4000));

  // Removing a client ends its tokens.
  auto t3 = code_flow();
  EXPECT_TRUE(id->remove_client("playground", "dumb-terminal", kNow + 3000));
  EXPECT_FALSE(id->bearer("playground", t3->access_token, kNow + 4000));
}

TEST_F(OAuthTest, DeviceFlowIsForAgentsOnly) {
  OAuthError e;
  EXPECT_FALSE(id->start_device("playground", "dumb-terminal", kNow, &e));
  EXPECT_EQ(e.code, "unauthorized_client");
  std::string why;
  auto agent = id->register_client("playground", "Grok", {"https://grok.example/cb"}, kNow, &why);
  ASSERT_TRUE(agent) << why;
  EXPECT_FALSE(agent->person);
  EXPECT_EQ(agent->source, "dynamic");
  EXPECT_TRUE(agent->client_id.starts_with("dyn_"));

  auto start = id->start_device("playground", agent->client_id, kNow, &e);
  ASSERT_TRUE(start) << e.description;
  EXPECT_TRUE(std::regex_match(start->user_code, std::regex("[B-Z]{4}-[B-Z]{4}")));
  EXPECT_FALSE(id->poll_device("playground", agent->client_id, start->device_code, kNow + 1000, kDay, &e));
  EXPECT_EQ(e.code, "authorization_pending");
  EXPECT_FALSE(id->poll_device("playground", agent->client_id, start->device_code, kNow + 2000, kDay, &e));
  EXPECT_EQ(e.code, "slow_down");

  // The person types the code, lower case and without the dash.
  std::string typed = start->user_code.substr(0, 4) + start->user_code.substr(5);
  for (auto& c : typed) c = char(std::tolower(c));
  auto shown = id->device_client("playground", typed, kNow + 3000);
  ASSERT_TRUE(shown);
  EXPECT_EQ(shown->name, "Grok");
  ASSERT_TRUE(id->decide_device("playground", typed, user_id, true, kNow + 4000, &why)) << why;
  EXPECT_FALSE(id->decide_device("playground", typed, user_id, true, kNow + 4000, &why));

  auto t = id->poll_device("playground", agent->client_id, start->device_code, kNow + 10000, kDay, &e);
  ASSERT_TRUE(t) << e.description;
  auto b = id->bearer("playground", t->access_token, kNow + 11000);
  ASSERT_TRUE(b);
  EXPECT_TRUE(b->agent);
  EXPECT_FALSE(id->poll_device("playground", agent->client_id, start->device_code, kNow + 20000, kDay, &e));

  // A denied or expired sign-in gives nothing.
  auto s2 = id->start_device("playground", agent->client_id, kNow, &e);
  ASSERT_TRUE(id->decide_device("playground", s2->user_code, user_id, false, kNow + 1000, &why));
  EXPECT_FALSE(id->poll_device("playground", agent->client_id, s2->device_code, kNow + 10000, kDay, &e));
  EXPECT_EQ(e.code, "access_denied");
  auto s3 = id->start_device("playground", agent->client_id, kNow, &e);
  EXPECT_FALSE(id->poll_device("playground", agent->client_id, s3->device_code, kNow + kDeviceCodeTtlMs, kDay, &e));
  EXPECT_EQ(e.code, "expired_token");
}

TEST_F(OAuthTest, ClientsAreChecked) {
  std::string why;
  EXPECT_FALSE(id->put_client("playground", Client{"bad id!", "x", {kAppRedirect}, false, "", 0}, kNow, &why));
  EXPECT_FALSE(id->put_client("playground", Client{"goober-sign-in", "x", {kAppRedirect}, false, "", 0}, kNow, &why));
  EXPECT_FALSE(id->put_client("playground", Client{"x", "x", {}, true, "", 0}, kNow, &why));
  EXPECT_FALSE(id->put_client("playground", Client{"x", "x", {"http://evil.test/cb"}, false, "", 0}, kNow, &why));
  EXPECT_FALSE(id->register_client("playground", "a\nb", {kAppRedirect}, kNow, &why));
  // Nothing invisible or direction-changing: "Grok" must read as what it is.
  for (const char* sneaky : {"Grok\u202e (verified)", "Gr\u200bok", "Grok\u2066x\u2069", "\ufeffGrok", "Gr\u00adok"}) {
    EXPECT_FALSE(id->register_client("playground", sneaky, {kAppRedirect}, kNow, &why)) << sneaky;
  }
  EXPECT_TRUE(id->register_client("unicode", "Grök Café — 東京", {kAppRedirect}, kNow, &why)) << why;
  auto list = id->clients("playground");
  ASSERT_EQ(list.size(), 1u);
  EXPECT_TRUE(list[0].person);
}
