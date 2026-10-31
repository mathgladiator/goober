// DKIM verification (packages/mail_inbound/src/dkim.cpp): RFC 8463's own Ed25519 example, RSA
// vectors made with an independent signer (Python's cryptography, its canonicalization checked
// against the RFC's example), tampering, alignment, and the refusals.

#include <gtest/gtest.h>

#include <map>

#include "goober/mail_inbound/dkim.h"

using namespace goober::mail::inbound;

namespace {

// RFC 8463 Appendix A.
const char* kEdKey = "v=DKIM1; k=ed25519; p=11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=";
const char* kEdSig =
    " v=1; a=ed25519-sha256; c=relaxed/relaxed;\r\n d=football.example.com; i=@football.example.com;\r\n"
    " q=dns/txt; s=brisbane; t=1528637909; h=from : to :\r\n subject : date : message-id : from : subject : date;\r\n"
    " bh=2jUSOH9NhtVGCQWNr9BrIAPreKQjO6Sn7XIkfJVOzv8=;\r\n b=/gCrinpcQOoIfuHNQIbq4pgh9kyIK3AQUdt9OdqQehSwhEIug4D11Bus\r\n"
    " Fa3bT3FY5OsU7ZbnKELq+eXdp1Q1Dw==\r\n";
const char* kEdBody = "Hi.\r\n\r\nWe lost the game.  Are you hungry yet?\r\n\r\nJoe.\r\n";

Headers football(const std::string& sig = kEdSig) {
  return {{"DKIM-Signature", sig},
          {"From", " Joe SixPack <joe@football.example.com>\r\n"},
          {"To", " Suzie Q <suzie@shopping.example.net>\r\n"},
          {"Subject", " Is dinner ready?\r\n"},
          {"Date", " Fri, 11 Jul 2003 21:00:37 -0700 (PDT)\r\n"},
          {"Message-ID", " <20030712040037.46341.5F8J@football.example.com>\r\n"}};
}

// Signed by an independent signer (a 2048-bit key made for this test).
const char* kRsaKey =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAukpZgo75LZblJELocVAiE/5cXUBWeF/2wcvCEFuRzAnqOSZpsuIVyXiwNkzOsrVNZK41V/RgKExDv"
    "ALgN36BKuUZSLkKyI3h6FB0W8993EU5/0oy6WjvSX7lg5IQRs6Da5QfayhsjeNrx+nKQoQlpUMAkTg5U6eHLm70m6sb1Ypshw9ovti60FIQmvEnySAyefIy6E"
    "kAajnx2+ev3nOPp5KvLHIWN8rjs6TQlFn0OL3P7u09ThLWRMj66LnjDc/oVNu3bL71/7K8XQZZ1fEDV9cWKpsVFdCgQffNYO9Uhzt960cj8TLIOrKNPa9R9nY"
    "qxpwhcZNdyEK2lIehxg+i+wIDAQAB";
const char* kRelaxedSig =
    " v=1; a=rsa-sha256; c=relaxed/relaxed; d=example.org; s=r1;\r\n h=from:to:subject:message-id; "
    "bh=3iOXUfns0FiFQbMifK5wxbsGK8Y0ZgVU3+JIrqqjL5w=;\r\n b=eyqVy2WsmEgoEsTOQ2WqMsZrzX/0gmaMgvnltwNT1XPFx2nZx1oruMNI1qVMsz41"
    "MTC1qlxPSj4vOBrsnxbu0X56OGOSYJKHn1cSibZNPPu4BMYpOCYGk8yRi/bs5gRWSjvxIO83mDdHC3mf53lT/83rXCg67X2+LDhxtGOuuNHUr6z/gP3+xBaFX"
    "o1tcOIpnAM8defbBl/mV5t2VyYINQTpw0EKjrdtQk7zIHdb/n2Ctz7AKV/RL2ADyifv+8upfTmF0RFbOYxlhoF05ZUeFptjIiHOerqjN2pNi4XJQRKGAAcVkS"
    "Gtwnk9AVBE1JHEq0a4jj6XIIaLyhL6BK53oQ==\r\n";
const char* kSimpleSig =
    " v=1; a=rsa-sha256; c=simple/simple; d=example.org; s=r1;\r\n h=from:to:subject:message-id; "
    "bh=1SP7q5pEjSOBxNqUP1fTSOUf6h1AX/Q1TqfpcRvA1/s=;\r\n b=VN7eRk4ILgosa4dY3QWFaOEcLgVIoe3/dpE0IxD100MPx1vAPgS5n39Vjn5xvBHX"
    "HdSwSpsmzZ2L+OqEOuXKUYGIHC6CIPqXh1iL5Y32oaTVZWCYf65pqHnRLLE3rT6O/0pSQEOvecSO5ytYR5wMxvW8pQtRb9oLxcF7q5ky8aDSuqqTPo8FvJZjE"
    "ETWUyWnHcZsGeMXhrdFehvIvEEu7RWZcvzxrrVno8rtlZRJ6lqlpNLRtIDRwpLK7QYJD+otlZCgjVwQYvZuDP/41eOzMOh1FLWg38mafiJZK2SqxJMi1IIahV9"
    "CLv505V0LOFgYLDSF1F0rjmuRtgqURUpZrg==\r\n";
const char* kRsaBody = "Two pounds,  please.\t \r\n\r\nAda\r\n\r\n\r\n";

Headers ada(const std::string& sig) {
  return {{"Received", " from mx.example.org by example.com\r\n"},
          {"DKIM-Signature", sig},
          {"From", " Ada Example <Ada@Example.ORG>\r\n"},
          {"To", " agent@example.com\r\n"},
          {"Subject", " Re:  the\t brisket\r\n  order\r\n"},
          {"Message-ID", " <m1@example.org>\r\n"}};
}

KeyLookup dns(std::map<std::string, std::string> records) {
  return [records](const std::string& name, std::string* error) -> std::optional<std::vector<std::string>> {
    auto it = records.find(name);
    if (it == records.end()) return *error = "NXDOMAIN", std::nullopt;
    return std::vector<std::string>{it->second};
  };
}

const int64_t kNow = 1793400000;

}  // namespace

TEST(Dkim, TheRfc8463Ed25519ExampleVerifies) {
  auto lookup = dns({{"brisbane._domainkey.football.example.com", kEdKey}});
  auto r = verify_dkim(football(), kEdBody, lookup, kNow);
  ASSERT_EQ(r.size(), 1u);
  EXPECT_TRUE(r[0].pass) << r[0].why;
  EXPECT_EQ(r[0].domain, "football.example.com");
  std::string why;
  EXPECT_EQ(verified_author(football(), kEdBody, lookup, kNow, &why), "joe@football.example.com") << why;
}

TEST(Dkim, IndependentRsaSignaturesVerifyInBothCanonicalizations) {
  auto lookup = dns({{"r1._domainkey.example.org", std::string("v=DKIM1; k=rsa; p=") + kRsaKey}});
  for (const char* sig : {kRelaxedSig, kSimpleSig}) {
    std::string why;
    // From's address comes back lowercased; the signer's domain is From's.
    EXPECT_EQ(verified_author(ada(sig), kRsaBody, lookup, kNow, &why), "ada@example.org") << sig << ": " << why;
  }
  // Simple canonicalization forgives nothing: one changed space in a signed header fails it.
  auto h = ada(kSimpleSig);
  h[4].second = " Re: the\t brisket\r\n  order\r\n";
  std::string why;
  EXPECT_EQ(verified_author(h, kRsaBody, lookup, kNow, &why), "");
  EXPECT_EQ(verified_author(ada(kRelaxedSig), kRsaBody, lookup, kNow, &why), "ada@example.org");
}

TEST(Dkim, TamperingFails) {
  auto lookup = dns({{"brisbane._domainkey.football.example.com", kEdKey}});
  auto r = verify_dkim(football(), "Hi.\r\n\r\nWe won the game.\r\n", lookup, kNow);
  EXPECT_FALSE(r.at(0).pass);
  EXPECT_NE(r.at(0).why.find("body hash"), std::string::npos) << r.at(0).why;
  auto h = football();
  h[3].second = " Is dinner ready? Send money.\r\n";
  r = verify_dkim(h, kEdBody, lookup, kNow);
  EXPECT_FALSE(r.at(0).pass);
  EXPECT_NE(r.at(0).why.find("doesn't verify"), std::string::npos) << r.at(0).why;
  // Relaxed canonicalization forgives whitespace a relay changed.
  h = football();
  h[3].second = " Is   dinner\r\n\tready?  \r\n";
  EXPECT_TRUE(verify_dkim(h, kEdBody, lookup, kNow).at(0).pass);
}

TEST(Dkim, OnlyAnAlignedPassMakesAVerifiedAuthor) {
  std::string why;
  // No key in DNS: nothing verifies.
  EXPECT_EQ(verified_author(football(), kEdBody, dns({}), kNow, &why), "");
  EXPECT_NE(why.find("no DKIM key"), std::string::npos) << why;
  auto lookup = dns({{"brisbane._domainkey.football.example.com", kEdKey}});
  // A second From header, unsigned, on top: refused outright, whatever the signature says.
  auto h = football();
  h.insert(h.begin(), {"From", " Boss <boss@football.example.com>\r\n"});
  EXPECT_EQ(verified_author(h, kEdBody, lookup, kNow, &why), "");
  EXPECT_EQ(why, "more than one From header");
  // No signature at all.
  h = football();
  h.erase(h.begin());
  EXPECT_EQ(verified_author(h, kEdBody, lookup, kNow, &why), "");
  EXPECT_EQ(why, "the message has no DKIM signature");
  // Relaxed alignment: the same domain, or From's a subdomain of the signer's.
  EXPECT_TRUE(dkim_aligned("example.org", "mail.example.org"));
  EXPECT_TRUE(dkim_aligned("Example.org", "example.ORG"));
  EXPECT_FALSE(dkim_aligned("mail.example.org", "example.org"));
  EXPECT_FALSE(dkim_aligned("ample.org", "example.org"));
  EXPECT_FALSE(dkim_aligned("org", "example.org"));
}

TEST(Dkim, KeysAndSignaturesThatAreRefused) {
  std::string err;
  std::string base = kEdSig;
  auto with = [&](const std::string& from, const std::string& to) {
    std::string s = base;
    s.replace(s.find(from), from.size(), to);
    return s;
  };
  EXPECT_FALSE(parse_dkim_signature(with("a=ed25519-sha256", "a=rsa-sha1"), &err));
  EXPECT_NE(err.find("rsa-sha1"), std::string::npos) << err;
  EXPECT_FALSE(parse_dkim_signature(with("q=dns/txt;", "q=dns/txt; l=10;"), &err));
  EXPECT_NE(err.find("l="), std::string::npos) << err;
  EXPECT_FALSE(parse_dkim_signature(with("h=from : to :\r\n subject : date : message-id : from : subject : date;",
                                         "h=to : subject;"),
                                    &err));
  EXPECT_NE(err.find("From"), std::string::npos) << err;
  EXPECT_FALSE(parse_dkim_signature(with("q=dns/txt;", "q=dns/txt; d=evil.example;"), &err));
  EXPECT_NE(err.find("twice"), std::string::npos) << err;
  // Expired (checked before the signature, which the edit also breaks).
  auto lookup = dns({{"brisbane._domainkey.football.example.com", kEdKey}});
  auto r = verify_dkim(football(with("q=dns/txt;", "q=dns/txt; x=1528637999;")), kEdBody, lookup, kNow);
  EXPECT_FALSE(r.at(0).pass);
  EXPECT_NE(r.at(0).why.find("expired"), std::string::npos) << r.at(0).why;
  // A key in testing mode, or revoked, counts as unsigned.
  r = verify_dkim(football(), kEdBody,
                  dns({{"brisbane._domainkey.football.example.com",
                        "v=DKIM1; k=ed25519; t=y; p=11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo="}}),
                  kNow);
  EXPECT_NE(r.at(0).why.find("testing"), std::string::npos) << r.at(0).why;
  r = verify_dkim(football(), kEdBody, dns({{"brisbane._domainkey.football.example.com", "v=DKIM1; k=ed25519; p="}}), kNow);
  EXPECT_NE(r.at(0).why.find("revoked"), std::string::npos) << r.at(0).why;
}

TEST(Dkim, Canonicalization) {
  EXPECT_EQ(canonical_body("", false), "\r\n");
  EXPECT_EQ(canonical_body("", true), "");
  EXPECT_EQ(canonical_body("a \t b  \r\n\r\n\r\n", true), "a b\r\n");
  EXPECT_EQ(canonical_body("a \t b  \r\n\r\n\r\n", false), "a \t b  \r\n");
  EXPECT_EQ(canonical_body("no end", true), "no end\r\n");
  EXPECT_EQ(canonical_header("Subject", " Hello\r\n  World \r\n", true), "subject:Hello World\r\n");
  EXPECT_EQ(canonical_header("Subject", " Hello\r\n  World \r\n", false), "Subject: Hello\r\n  World \r\n");
}
