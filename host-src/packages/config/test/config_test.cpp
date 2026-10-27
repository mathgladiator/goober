#include <gtest/gtest.h>

#include "goober/config/config.h"

using namespace goober::config;

namespace {
const char* kGood = R"({
  "listen": {"address": "0.0.0.0", "port": 9090},
  "storage": "data",
  "platform-domain": "Example.COM",
  "domains": {"LocalHost": {"tenant": "playground"}},
  "operations": {"token": "0123456789abcdef"},
  "threads": {"network": 1, "boxes": 2, "pin-boxes": true},
  "limits": {"timeout-ms": {"default": 50, "max": 100}}
})";
}

TEST(Config, ParsesEverything) {
  std::string err;
  auto cfg = parse_config(kGood, "/srv", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_EQ(cfg->listen_port, 9090);
  EXPECT_EQ(cfg->storage, "/srv/data");
  EXPECT_EQ(cfg->platform_domain, "example.com");
  EXPECT_EQ(*cfg->tenant_for_host("localhost:9090"), "playground");
  EXPECT_EQ(*cfg->tenant_for_host("LOCALHOST"), "playground");
  EXPECT_FALSE(cfg->tenant_for_host("other").has_value());
  EXPECT_EQ(cfg->timeout_ms.resolve(std::nullopt), 50);
  EXPECT_EQ(cfg->timeout_ms.resolve(500), 100);
  EXPECT_EQ(cfg->timeout_ms.resolve(0), 1);
  EXPECT_TRUE(cfg->pin_boxes);
}

TEST(Config, RefusesUnknownFieldsAndBadValues) {
  std::string err;
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"nope":1})", "/", &err));
  EXPECT_NE(err.find("nope"), std::string::npos);
  err.clear();
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"short"}})", "/", &err));
  err.clear();
  EXPECT_FALSE(parse_config(
      R"({"operations":{"token":"0123456789abcdef"},"domains":{"a.com":{"tenant":"Bad-Name"}}})", "/", &err));
  err.clear();
  EXPECT_FALSE(parse_config(
      R"({"operations":{"token":"0123456789abcdef"},"limits":{"fuel":{"default":10,"max":5}}})", "/", &err));
}

TEST(Config, ReloadKeepsRestartOnlyFields) {
  std::string err;
  ConfigHolder holder(parse_config(kGood, "/srv", &err));
  auto next = parse_config(R"({"listen":{"port":1},"operations":{"token":"fedcba9876543210"}})", "/srv", &err);
  ASSERT_TRUE(next) << err;
  std::string notes = holder.replace(next);
  EXPECT_NE(notes.find("listen.port"), std::string::npos);
  EXPECT_EQ(holder.get()->listen_port, 9090);
  EXPECT_EQ(holder.get()->operations_token, "fedcba9876543210");
}

TEST(Config, ReadsIdentityOwnersAndPublicUrl) {
  std::string err;
  auto cfg = parse_config(R"({
    "public-url": "https://goober.example.com/",
    "domains": {"bobs.example": {"tenant": "bobs", "owner": "Bob@Example.com"}},
    "identity": {"cookie-secure": true, "session-days": 7,
                 "operators": {"Me@Example.com": ["host:owner"]}}
  })", "/srv", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_TRUE(cfg->operations_token.empty());
  EXPECT_EQ(cfg->public_url, "https://goober.example.com");
  EXPECT_EQ(cfg->base_url(), "https://goober.example.com");
  EXPECT_EQ(cfg->domains.at("bobs.example").owner, "bob@example.com");
  EXPECT_EQ(cfg->identity.session_days, 7);
  EXPECT_EQ(cfg->identity.operators.at("me@example.com").at(0), "host:owner");

  EXPECT_FALSE(parse_config(R"({"identity":{"operators":{"me@example.com":["owner"]}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"domains":{"a.com":{"tenant":"a","owner":"nope"}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"identity":{"mailer":"ses"}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"public-url":"goober.example.com"})", "/", &err));
}

TEST(Config, ReadsOAuthClients) {
  std::string err;
  auto cfg = parse_config(R"({"identity": {"clients": {"dumb-terminal": {"name": "dumb terminal",
      "redirect-uris": ["com.mathgladiator.dumb:/oauth/callback", "http://127.0.0.1/callback"], "acts-as": "person"}}}})",
                          "/", &err);
  ASSERT_TRUE(cfg) << err;
  const auto& c = cfg->identity.clients.at("dumb-terminal");
  EXPECT_TRUE(c.person);
  EXPECT_EQ(c.redirect_uris.size(), 2u);
  EXPECT_FALSE(parse_config(R"({"identity":{"clients":{"x":{"name":"x","redirect-uris":[],"acts-as":"person"}}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"identity":{"clients":{"x":{"name":"x","redirect-uris":["a.b:/c"],"acts-as":"robot"}}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"identity":{"clients":{"x":{"name":"x","redirect-uris":["a.b:/c"],"acts-as":"agent","secret":"s"}}}})", "/", &err));
}

TEST(Config, LocalModeAliasesDomainsUnderLocalhost) {
  std::string err;
  auto cfg = parse_config(R"({
    "listen": {"address": "0.0.0.0", "port": 9090},
    "public-url": "https://goober.example.com",
    "platform-domain": "goober.example.com",
    "domains": {"goober.example.com": {"tenant": "home"}, "bobs.example": {"tenant": "bobs"}}
  })", "/srv", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_FALSE(cfg->tenant_for_host("bobs.example.localhost").has_value());

  auto local = local_mode(cfg);
  EXPECT_EQ(local->listen_address, "127.0.0.1");
  EXPECT_FALSE(local->identity.cookie_secure);
  EXPECT_EQ(local->base_url(), "http://127.0.0.1:9090");
  EXPECT_EQ(*local->tenant_for_host("bobs.example.localhost:9090"), "bobs");
  EXPECT_EQ(local->resolve_host("bobs.example.localhost")->first, "bobs.example");
  EXPECT_EQ(*local->tenant_for_host("localhost:9090"), "home");
  EXPECT_EQ(*local->tenant_for_host("127.0.0.1"), "home");
  EXPECT_EQ(*local->tenant_for_host("10.0.2.2:9090"), "home");  // the Android emulator
  EXPECT_EQ(*local->tenant_for_host("[::1]:9090"), "home");
  EXPECT_FALSE(cfg->tenant_for_host("10.0.2.2").has_value());
  EXPECT_FALSE(local->tenant_for_host("nope.localhost").has_value());
  EXPECT_FALSE(local->tenant_for_host("other.example").has_value());
}
