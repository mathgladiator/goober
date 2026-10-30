#include <gtest/gtest.h>

#include "goober/signing/oauth1.h"

using namespace goober::signing;

// X's documented example of signing a request (developer.x.com, "Creating a signature").
TEST(OAuth1, MatchesXsPublishedSignature) {
  OAuth1 o{"xvz1evFS4wEEPTGEFPHBog", "kAcSOqF21Fu85e7zjz7ZN2U4ZRhfV3WpwPAoE3Z7kBw",
           "370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb", "LswwdoUaIvS8ltyTt5jkRh4J50vUPVVHtR2YPi5kE",
           "kYjzVBB8Y0ZFabxSWbWovY3uYSQ2pTgmZeNu2VS4cg", "1318622958"};
  std::string base = oauth1_base_string("post", "https://api.twitter.com/1.1/statuses/update.json?include_entities=true",
                                        {{"status", "Hello Ladies + Gentlemen, a signed OAuth request!"}}, o);
  EXPECT_EQ(base,
            "POST&https%3A%2F%2Fapi.twitter.com%2F1.1%2Fstatuses%2Fupdate.json&include_entities%3Dtrue%26oauth_consumer_key%3D"
            "xvz1evFS4wEEPTGEFPHBog%26oauth_nonce%3DkYjzVBB8Y0ZFabxSWbWovY3uYSQ2pTgmZeNu2VS4cg%26oauth_signature_method%3DHMAC-"
            "SHA1%26oauth_timestamp%3D1318622958%26oauth_token%3D370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb%26oauth_"
            "version%3D1.0%26status%3DHello%2520Ladies%2520%252B%2520Gentlemen%252C%2520a%2520signed%2520OAuth%2520request%2521");
  EXPECT_EQ(oauth1_signature(base, o), "hCtSmYh+iHYCEqBWrE7C7hYmtUk=");
  std::string h = oauth1_header("POST", "https://api.twitter.com/1.1/statuses/update.json?include_entities=true",
                                {{"status", "Hello Ladies + Gentlemen, a signed OAuth request!"}}, o);
  EXPECT_NE(h.find("oauth_signature=\"hCtSmYh%2BiHYCEqBWrE7C7hYmtUk%3D\""), std::string::npos) << h;
  EXPECT_TRUE(h.starts_with("OAuth oauth_consumer_key=\"xvz1evFS4wEEPTGEFPHBog\""));
  EXPECT_EQ(rfc3986("a b+~*"), "a%20b%2B~%2A");
}
