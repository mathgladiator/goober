#include <gtest/gtest.h>

#include <regex>

#include "goober/identity/identity.h"

using namespace goober::identity;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kNow = 1'800'000'000'000;
constexpr int64_t kDay = 24LL * 3600 * 1000;

class IdentityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = fs::path(GOOBER_TEST_TMP) / "identity";
    fs::remove_all(dir);
    id = std::make_unique<Identity>(dir, [this](const Mail& m) { mail.push_back(m); });
  }
  // The code in the newest mail.
  std::string last_code() {
    std::smatch m;
    EXPECT_FALSE(mail.empty());
    EXPECT_TRUE(std::regex_search(mail.back().text, m, std::regex("is ([0-9]{6})")));
    return m[1];
  }
  fs::path dir;
  std::vector<Mail> mail;
  std::unique_ptr<Identity> id;
};
}  // namespace

TEST_F(IdentityTest, EmailCodeSignsInAndOpensASession) {
  id->start_email("playground", " Alice@Example.com ", "localhost", true, kNow);
  ASSERT_EQ(mail.size(), 1u);
  EXPECT_EQ(mail[0].to, "alice@example.com");
  EXPECT_NE(mail[0].subject.find("localhost"), std::string::npos);

  std::string why;
  auto in = id->verify_email("playground", "alice@example.com", last_code(), true, kNow + 1000, kDay, &why);
  ASSERT_TRUE(in) << why;
  EXPECT_TRUE(in->created);
  EXPECT_EQ(in->token.size(), 64u);
  EXPECT_TRUE(fs::exists(dir / "playground.db"));

  auto who = id->session("playground", in->token, kNow + 2000);
  ASSERT_TRUE(who);
  EXPECT_EQ(who->email, "alice@example.com");
  EXPECT_EQ(who->user_id, in->user.user_id);
  // A session belongs to its realm, expires, and ends.
  EXPECT_FALSE(id->session("second", in->token, kNow + 2000));
  EXPECT_FALSE(id->session("playground", in->token, kNow + kDay + 1001));
  id->end_session("playground", in->token, kNow + 3000);
  EXPECT_FALSE(id->session("playground", in->token, kNow + 4000));
}

TEST_F(IdentityTest, CodesAreSingleUseAndLockAfterFailures) {
  id->start_email("playground", "bob@example.com", "localhost", true, kNow);
  std::string code = last_code();
  std::string why;
  EXPECT_FALSE(id->verify_email("playground", "bob@example.com", "000000" == code ? "111111" : "000000", true,
                                kNow, kDay, &why));
  ASSERT_TRUE(id->verify_email("playground", "bob@example.com", code, true, kNow, kDay, &why)) << why;
  EXPECT_FALSE(id->verify_email("playground", "bob@example.com", code, true, kNow, kDay, &why));

  id->start_email("playground", "bob@example.com", "localhost", true, kNow);
  code = last_code();
  for (int i = 0; i < kCodeMaxAttempts; ++i) {
    EXPECT_FALSE(id->verify_email("playground", "bob@example.com", "x", true, kNow, kDay, &why));
  }
  EXPECT_FALSE(id->verify_email("playground", "bob@example.com", code, true, kNow, kDay, &why));
  EXPECT_NE(why.find("Too many"), std::string::npos);
  // Expired codes don't work.
  id->start_email("playground", "bob@example.com", "localhost", true, kNow);
  EXPECT_FALSE(id->verify_email("playground", "bob@example.com", last_code(), true, kNow + kCodeTtlMs + 1, kDay, &why));
}

TEST_F(IdentityTest, WithoutSignupOnlyExistingUsersGetCodes) {
  id->start_email("_host", "stranger@example.com", "localhost", false, kNow);
  EXPECT_TRUE(mail.empty());
  std::string why;
  ASSERT_TRUE(id->grant("playground", "owner@example.com", "owner", "test", kNow, &why)) << why;
  id->start_email("playground", "owner@example.com", "localhost", false, kNow);
  ASSERT_EQ(mail.size(), 1u);
  auto in = id->verify_email("playground", "owner@example.com", last_code(), false, kNow, kDay, &why);
  ASSERT_TRUE(in) << why;
  EXPECT_FALSE(in->created);
  EXPECT_EQ(in->user.roles, std::vector<std::string>{"owner"});
}

TEST_F(IdentityTest, StartsAreThrottledPerAddress) {
  for (int i = 0; i < kCodesPerWindow + 3; ++i) id->start_email("playground", "c@example.com", "x", true, kNow);
  EXPECT_EQ(mail.size(), size_t(kCodesPerWindow));
  id->start_email("playground", "not an email", "x", true, kNow);
  EXPECT_EQ(mail.size(), size_t(kCodesPerWindow));
}

TEST_F(IdentityTest, GrantsRevokesAndAudit) {
  std::string why;
  EXPECT_FALSE(id->grant("playground", "d@example.com", "host:owner", "test", kNow, &why));
  EXPECT_FALSE(id->grant("playground", "d@example.com", "a/b/c", "test", kNow, &why));
  EXPECT_FALSE(id->grant("_host", "d@example.com", "admin", "test", kNow, &why));
  ASSERT_TRUE(id->grant("playground", "d@example.com", "admin/project_42", "test", kNow, &why)) << why;
  ASSERT_TRUE(id->grant("playground", "d@example.com", "member", "test", kNow, &why));
  EXPECT_EQ(id->holders("playground", "member"), 1);
  EXPECT_EQ(id->holders("playground", "owner"), 0);
  EXPECT_TRUE(id->revoke("playground", "d@example.com", "member", "test", kNow, &why));
  EXPECT_FALSE(id->revoke("playground", "d@example.com", "member", "test", kNow, &why));

  id->start_email("playground", "d@example.com", "x", true, kNow);
  auto in = id->verify_email("playground", "d@example.com", last_code(), true, kNow, kDay, &why);
  ASSERT_TRUE(in);
  EXPECT_EQ(in->user.roles, std::vector<std::string>{"admin/project_42"});
  EXPECT_EQ(id->revoke_sessions("playground", "d@example.com", "test", kNow), 1);
  EXPECT_FALSE(id->session("playground", in->token, kNow));

  auto d = id->describe("playground", kNow);
  EXPECT_EQ(d.at("users").as_array().size(), 1u);
  EXPECT_GE(d.at("audit").as_array().size(), 5u);
}

TEST(IdentityNames, RolesAndRealms) {
  EXPECT_TRUE(is_valid_tenant_role("owner"));
  EXPECT_TRUE(is_valid_tenant_role("admin/project_42"));
  EXPECT_FALSE(is_valid_tenant_role("admin/"));
  EXPECT_FALSE(is_valid_tenant_role("host:owner"));
  EXPECT_FALSE(is_valid_tenant_role("a/b/c"));
  EXPECT_TRUE(is_valid_realm("_host"));
  EXPECT_TRUE(is_valid_realm("playground"));
  EXPECT_FALSE(is_valid_realm("../etc"));
}
