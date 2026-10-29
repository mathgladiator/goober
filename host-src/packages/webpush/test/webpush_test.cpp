#include <gtest/gtest.h>

#include "goober/webpush/webpush.h"

using namespace goober::webpush;

namespace {
std::string d(const char* s) { return *b64url_decode(s); }
}  // namespace

// RFC 8291 Appendix A: the worked example, byte for byte.
TEST(WebPush, TheRfc8291ExampleEncryptsExactly) {
  std::string err;
  auto as = key_from_private(d("yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw"), &err);
  ASSERT_TRUE(as) << err;
  EXPECT_EQ(b64url(as->public_raw),
            "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8");
  auto ua = key_from_private(d("q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94"), &err);
  ASSERT_TRUE(ua) << err;
  EXPECT_EQ(b64url(ua->public_raw),
            "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4");
  std::string auth = d("BTBZMqHH6r4Tts7J_aSIgg");
  auto body = encrypt("When I grow up, I want to be a watermelon", ua->public_raw, auth, &err,
                      d("DGv6ra1nlYgDCS1FRnbzlw"), as);
  ASSERT_TRUE(body) << err;
  EXPECT_EQ(b64url(*body),
            "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A_yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN");
  auto back = decrypt(*body, *ua, auth, &err);
  ASSERT_TRUE(back) << err;
  EXPECT_EQ(*back, "When I grow up, I want to be a watermelon");
}

TEST(WebPush, FreshKeysRoundTripAndTamperingIsCaught) {
  std::string err;
  auto ua = generate_key(&err);
  ASSERT_TRUE(ua) << err;
  EXPECT_EQ(ua->public_raw.size(), 65u);
  std::string auth(16, '\x07');
  auto body = encrypt(R"({"title":"Pickup ready","url":"/app/orders/7"})", ua->public_raw, auth, &err);
  ASSERT_TRUE(body) << err;
  EXPECT_EQ(*decrypt(*body, *ua, auth, &err), R"({"title":"Pickup ready","url":"/app/orders/7"})");
  std::string bad = *body;
  bad.back() ^= 1;
  EXPECT_FALSE(decrypt(bad, *ua, auth, &err));
  EXPECT_FALSE(encrypt(std::string(kMaxPlaintext + 1, 'x'), ua->public_raw, auth, &err));
  EXPECT_FALSE(encrypt("x", "short", auth, &err));
}

TEST(WebPush, VapidIsAnEs256JwtTheKeyVerifies) {
  std::string err;
  auto key = generate_key(&err);
  ASSERT_TRUE(key) << err;
  auto header = vapid_authorization(*key, "https://fcm.googleapis.com", "mailto:ops@kcmeatup.com", 1800000000, &err);
  ASSERT_TRUE(header) << err;
  ASSERT_TRUE(header->starts_with("vapid t="));
  auto comma = header->find(", k=");
  std::string jwt = header->substr(8, comma - 8);
  EXPECT_EQ(header->substr(comma + 4), b64url(key->public_raw));
  EXPECT_TRUE(verify_jwt(jwt, key->public_raw));
  auto other = generate_key(&err);
  EXPECT_FALSE(verify_jwt(jwt, other->public_raw));
  auto claims = *b64url_decode(jwt.substr(jwt.find('.') + 1, jwt.rfind('.') - jwt.find('.') - 1));
  EXPECT_EQ(claims, R"({"aud":"https://fcm.googleapis.com","exp":1800000000,"sub":"mailto:ops@kcmeatup.com"})");
}

TEST(WebPush, Base64UrlBothWays) {
  EXPECT_EQ(b64url("\xfb\xff"), "-_8");
  EXPECT_EQ(*b64url_decode("-_8"), "\xfb\xff");
  EXPECT_FALSE(b64url_decode("a"));
  EXPECT_FALSE(b64url_decode("a b"));
}
