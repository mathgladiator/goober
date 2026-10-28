#include <gtest/gtest.h>

#include "goober/mail_inbound/address.h"

using namespace goober::mail::inbound;

namespace {
AddressPattern parse(const char* s) {
  std::string err;
  auto p = parse_address_pattern(s, &err);
  EXPECT_TRUE(p) << s << ": " << err;
  return p.value_or(AddressPattern{});
}
}  // namespace

TEST(MailAddress, PatternsParseAndKeepTheirShape) {
  auto exact = parse("support@example.com");
  EXPECT_EQ(exact.domain, "example.com");
  EXPECT_FALSE(exact.catch_all);
  EXPECT_EQ(exact.shape(), "support@example.com");
  EXPECT_EQ(parse("support@").domain, "");
  EXPECT_TRUE(parse("*@").catch_all);
  EXPECT_EQ(parse("*@example.com").shape(), "*@example.com");
  auto reply = parse("reply+{ticket : token}@example.com");
  ASSERT_EQ(reply.local.size(), 2u);
  EXPECT_EQ(reply.local[1].text, "ticket");
  EXPECT_EQ(reply.local[1].type, "token");
  EXPECT_EQ(reply.token_param(), "ticket");
  EXPECT_EQ(reply.shape(), "reply+{}@example.com");
  EXPECT_EQ(parse("orders+{id}@").shape(), parse("orders+{order}@").shape());
}

TEST(MailAddress, BadPatternsAreRefused) {
  auto refuse = [](const char* s, const char* why) {
    std::string err;
    EXPECT_FALSE(parse_address_pattern(s, &err)) << s;
    EXPECT_NE(err.find(why), std::string::npos) << s << ": " << err;
  };
  refuse("support", "local@domain");
  refuse("Support@example.com", "can't be in a local part");
  refuse("support@Example.com", "lowercase domain");
  refuse("support@localhost", "lowercase domain");
  refuse("@example.com", "1 to 64");
  refuse(".a@", "dot");
  refuse("a..b@", "dot");
  refuse("{a}{b}@", "literal between");
  refuse("x+{a}+{a}@", "twice");
  refuse("x+{Bad}@", "must match");
  refuse("x+{a : int}@", "token, or none");
  refuse("x+{a : token}+{b : token}@", "at most one token");
  refuse("x+{a@", "without its }");
  refuse("a b@", "can't be in a local part");
}

TEST(MailAddress, LocalPartsMatchWithParameters) {
  auto reply = parse("reply+{ticket : token}@example.com");
  auto m = match_local(reply, "reply+abc123");
  ASSERT_TRUE(m);
  EXPECT_EQ(m->at("ticket"), "abc123");
  EXPECT_FALSE(match_local(reply, "reply+"));
  EXPECT_FALSE(match_local(reply, "reply"));
  EXPECT_FALSE(match_local(reply, "replyx+abc"));
  EXPECT_FALSE(match_local(reply, "reply+ab!c"));
  auto two = parse("{who}.{team}@");
  auto t = match_local(two, "ann.sales");
  ASSERT_TRUE(t);
  EXPECT_EQ(t->at("who"), "ann");
  EXPECT_EQ(t->at("team"), "sales");
  EXPECT_TRUE(match_local(parse("support@"), "support"));
  EXPECT_FALSE(match_local(parse("support@"), "supports"));
  EXPECT_TRUE(match_local(parse("*@"), "anything+at.all"));
}

TEST(MailAddress, SplitsPlainAddresses) {
  auto a = split_address("Ann.Lee@Example.COM");
  ASSERT_TRUE(a);
  EXPECT_EQ(a->first, "ann.lee");
  EXPECT_EQ(a->second, "example.com");
  EXPECT_FALSE(split_address("no-at-sign"));
  EXPECT_FALSE(split_address("a@b"));
  EXPECT_FALSE(split_address("a b@example.com"));
  EXPECT_FALSE(split_address("@example.com"));
}
