// Deleting an account (design/machine/021.IdentityEvents.md § 6): everything that acts as the
// person ends, the address leaves the realm, the id stays a tombstone, a "deleted" event without
// the address is appended, and the address may sign up again as someone new.

#include <gtest/gtest.h>

#include <regex>

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

class DeletionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = fs::path(GOOBER_TEST_TMP) / "identity_deletion";
    fs::remove_all(dir);
    id = std::make_unique<Identity>(dir, [this](const Mail& m) { mail.push_back(m); });
    Client terminal{"dumb-terminal", "dumb terminal", {kAppRedirect}, true, "config", 0};
    std::string why;
    ASSERT_TRUE(id->put_client("playground", terminal, kNow, &why)) << why;
  }
  std::optional<SignIn> sign_in(const std::string& email, int64_t at = kNow) {
    id->start_email("playground", email, "localhost", true, at);
    std::smatch m;
    EXPECT_TRUE(std::regex_search(mail.back().text, m, std::regex("is ([0-9]{6})")));
    std::string why;
    auto in = id->verify_email("playground", email, std::string(m[1]), true, at, kDay, &why);
    EXPECT_TRUE(in) << why;
    return in;
  }
  fs::path dir;
  std::vector<Mail> mail;
  std::unique_ptr<Identity> id;
};
}  // namespace

TEST_F(DeletionTest, ErasesTheAccountKeepsTheIdAndTellsBoxes) {
  auto in = sign_in("ada@example.com");
  ASSERT_TRUE(in);
  int64_t ada = in->user.user_id;
  std::string why;
  ASSERT_TRUE(id->grant("playground", "ada@example.com", "member", "operator", kNow, &why)) << why;
  // A grant to an agent, by OAuth.
  std::string code = id->issue_code("playground", "dumb-terminal", ada, kAppRedirect, kChallenge, kNow);
  OAuthError e;
  auto tokens = id->exchange_code("playground", "dumb-terminal", code, kAppRedirect, kVerifier, kNow + 1, 30 * kDay, &e);
  ASSERT_TRUE(tokens) << e.description;
  ASSERT_TRUE(id->bearer("playground", tokens->access_token, kNow + 2));
  // An open invite to someone else that Ada sent, and one to her address from before.
  Invite to_bo;
  to_bo.email = "bo@example.com";
  to_bo.inviter_email = "ada@example.com";
  to_bo.inviter_kind = "person";
  std::string invite_code;
  ASSERT_TRUE(id->create_invite("playground", to_bo, kNow, &invite_code, &why)) << why;
  int64_t last_before = id->last_event_id("playground");

  auto gone = id->delete_user("playground", ada, "self", "user:" + std::to_string(ada), kNow + kDay, &why);
  ASSERT_TRUE(gone) << why;
  EXPECT_EQ(gone->user_id, ada);
  EXPECT_EQ(gone->how, "self");

  // Nothing acts as her any more.
  EXPECT_FALSE(id->session("playground", in->token, kNow + kDay + 1));
  EXPECT_FALSE(id->bearer("playground", tokens->access_token, kNow + kDay + 1));
  OAuthError re;
  EXPECT_FALSE(id->refresh("playground", "dumb-terminal", tokens->refresh_token, kNow + kDay + 1, 30 * kDay, &re));
  EXPECT_FALSE(id->user("playground", ada));
  EXPECT_FALSE(id->find_user("playground", "ada@example.com"));
  EXPECT_EQ(id->grants("playground", ada, kNow + kDay + 1).size(), 0u);
  EXPECT_EQ(id->holders("playground", "member"), 0);

  // The address is nowhere in the realm: not on the account, its events, its audit, or invites.
  auto realm = id->describe("playground", kNow + kDay + 1);
  EXPECT_EQ(json::dump(realm).find("ada@example.com"), std::string::npos) << json::dump(realm);
  for (const auto& ev : id->events_after("playground", 0, 100)) EXPECT_NE(ev.email, "ada@example.com");
  for (const auto& inv : id->invites("playground", "", "", "", 100)) {
    EXPECT_NE(inv.email, "ada@example.com");
    EXPECT_NE(inv.inviter_email, "ada@example.com");
  }

  // The event: "deleted", her id, no address; and a lasting record of it.
  auto events = id->events_after("playground", last_before, 10);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, kDeleted);
  EXPECT_EQ(events[0].user_id, ada);
  EXPECT_EQ(events[0].email, "");
  EXPECT_EQ(events[0].id, gone->event_id);
  auto list = id->deletions("playground", 10);
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0].user_id, ada);
  // Events are pruned after a week; the record of the deletion isn't.
  id->prune("playground", kNow + 60 * kDay);
  EXPECT_EQ(id->deletions("playground", 10).size(), 1u);

  // Once only.
  EXPECT_FALSE(id->delete_user("playground", ada, "operator", "config", kNow + kDay + 2, &why));
  EXPECT_NE(why.find("already deleted"), std::string::npos);

  // The same address signs up again as somebody new, with a new id and no roles.
  auto again = sign_in("ada@example.com", kNow + 2 * kDay);
  ASSERT_TRUE(again);
  EXPECT_TRUE(again->created);
  EXPECT_NE(again->user.user_id, ada);
  EXPECT_GT(again->user.user_id, ada);
  EXPECT_TRUE(again->user.roles.empty());
}

TEST_F(DeletionTest, RefusesTheHostRealmAndStrangers) {
  std::string why;
  EXPECT_FALSE(id->delete_user(kHostRealm, 1, "operator", "config", kNow, &why));
  EXPECT_NE(why.find("identity.operators"), std::string::npos);
  EXPECT_FALSE(id->delete_user("playground", 999, "operator", "config", kNow, &why));
  EXPECT_FALSE(id->delete_user("playground", 999, "maybe", "config", kNow, &why));
}
