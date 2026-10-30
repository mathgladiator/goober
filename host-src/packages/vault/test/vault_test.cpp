#include <gtest/gtest.h>

#include "goober/vault/vault.h"

using namespace goober::vault;

TEST(Vault, SealsAndOpensOnlyInItsContext) {
  Key root{};
  root[0] = 7;
  auto kc = tenant_key(root, "kc"), other = tenant_key(root, "other");
  EXPECT_NE(kc, other);
  auto ctx = context("kc", "hevy", "42");
  auto s = seal(kc, R"({"kind":"key","value":"sk-123"})", ctx);
  EXPECT_TRUE(s.starts_with("gsv1."));
  EXPECT_EQ(s.find("sk-123"), std::string::npos);
  EXPECT_EQ(*unseal(kc, s, ctx), R"({"kind":"key","value":"sk-123"})");
  EXPECT_NE(seal(kc, "x", ctx), seal(kc, "x", ctx));  // a fresh salt each time
  EXPECT_FALSE(unseal(other, s, ctx));                     // another tenant's key
  EXPECT_FALSE(unseal(kc, s, context("kc", "strava", "42")));  // another service
  EXPECT_FALSE(unseal(kc, s, context("kc", "hevy", "43")));    // another owner
  EXPECT_FALSE(unseal(kc, s, context("kc", "hev", "y42")));    // the same bytes, split otherwise
  std::string flipped = s;
  flipped[10] = flipped[10] == 'A' ? 'B' : 'A';
  EXPECT_FALSE(unseal(kc, flipped, ctx));
  EXPECT_FALSE(unseal(kc, "gsv1.", ctx));
  EXPECT_FALSE(unseal(kc, "sk-123", ctx));
}

TEST(Vault, TheOwnerTravelsInTheClearAndIsChecked) {
  Key root{};
  auto kc = tenant_key(root, "kc");
  auto s = seal_for(kc, "kc", "strava", "42", "secret");
  EXPECT_TRUE(s.starts_with("gsv1.42."));
  auto o = open_for(kc, "kc", "strava", s);
  ASSERT_TRUE(o);
  EXPECT_EQ(o->owner, "42");
  EXPECT_EQ(o->plaintext, "secret");
  std::string moved = s;
  moved.replace(5, 2, "43");  // claimed for another person
  EXPECT_FALSE(open_for(kc, "kc", "strava", moved));
  EXPECT_FALSE(open_for(kc, "kc", "hevy", s));
  EXPECT_FALSE(open_for(tenant_key(root, "other"), "other", "strava", s));
}
