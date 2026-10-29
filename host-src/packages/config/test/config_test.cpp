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
  EXPECT_FALSE(parse_config(R"({"identity":{"mailer":"smtp"}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"identity":{"mailer":"smtp","smtp":{"url":"http://x","from":"a@b.c"}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"identity":{"mailer":"smtp","smtp":{"url":"smtps://x;rm","from":"a@b.c"}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"identity":{"mailer":"smtp","smtp":{"url":"smtps://x","from":"a@b.c","username":"u"}}})", "/", &err));
  auto smtp = parse_config(R"({"identity":{"mailer":"smtp","smtp":{"url":"smtps://smtp.example.com:465",
      "from":"No-Reply@Example.com","username":"u","password-file":"/etc/goober/smtp"}}})", "/", &err);
  ASSERT_TRUE(smtp) << err;
  EXPECT_EQ(smtp->identity.smtp.from, "no-reply@example.com");
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

TEST(Config, ReadsSesBackupSourceAndSandbox) {
  std::string err;
  auto cfg = parse_config(R"({
    "sandbox": true,
    "aws": {"region": "us-west-2"},
    "identity": {"mailer": "ses", "ses": {"from": "No-Reply@Example.com", "configuration-set": "goober"}},
    "mail": {"daily-max": 50, "per-address-daily": 2},
    "backup": {"s3": {"bucket": "my-backups", "prefix": "host1"}, "nightly-at": "04:15", "keep-days": 7,
               "databases-every-ms": 600000},
    "source": {"vendor": "vendor", "sandbox": "none", "build-timeout-ms": 60000}
  })", "/srv", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_TRUE(cfg->sandbox);
  EXPECT_EQ(cfg->aws.region, "us-west-2");
  EXPECT_EQ(cfg->identity.mailer, "ses");
  EXPECT_EQ(cfg->identity.from_address(), "no-reply@example.com");
  EXPECT_EQ(cfg->identity.ses.configuration_set, "goober");
  EXPECT_EQ(cfg->mail.daily_max, 50);
  EXPECT_EQ(cfg->mail.per_address_daily, 2);
  EXPECT_TRUE(cfg->backup.enabled());
  EXPECT_EQ(cfg->backup.bucket, "my-backups");
  EXPECT_EQ(cfg->backup.prefix, "host1/");
  EXPECT_EQ(cfg->backup.nightly_minute, 4 * 60 + 15);
  EXPECT_EQ(cfg->backup.keep_days, 7);
  EXPECT_EQ(cfg->backup.databases_every_ms, 600000);
  EXPECT_EQ(cfg->source.vendor, "/srv/vendor");
  EXPECT_EQ(cfg->source.sandbox, "none");

  auto plain = parse_config("{}", "/srv", &err);
  ASSERT_TRUE(plain);
  EXPECT_FALSE(plain->backup.enabled());
  EXPECT_FALSE(plain->sandbox);
  EXPECT_EQ(plain->source.sandbox, "bwrap");
}

TEST(Config, RefusesBadSesBackupAndSource) {
  std::string err;
  for (const char* bad : {
           R"({"identity": {"mailer": "ses"}})",
           R"({"identity": {"mailer": "ses", "ses": {"from": "not-an-email"}}})",
           R"({"identity": {"ses": {"from": "a@b.c", "endpoint": "http://example.com"}}})",
           R"({"aws": {"region": "US East"}})",
           R"({"backup": {}})",
           R"({"backup": {"s3": {"bucket": "Bad_Bucket"}}})",
           R"({"backup": {"s3": {"bucket": "ok-bucket", "prefix": "../x"}}})",
           R"({"backup": {"s3": {"bucket": "ok-bucket"}, "nightly-at": "25:00"}})",
           R"({"backup": {"s3": {"bucket": "ok-bucket"}, "databases-every-ms": 1000}})",
           R"({"source": {"sandbox": "docker"}})",
           R"({"mail": {"per-address-daily": 0}})",
       }) {
    err.clear();
    EXPECT_FALSE(parse_config(bad, "/", &err)) << bad;
    EXPECT_FALSE(err.empty()) << bad;
  }
}

TEST(Config, ThePanelHasADomainOfItsOwn) {
  std::string err;
  auto cfg = parse_config(
      R"({"domains":{"kcmeatup.com":{"tenant":"kc"}},"operations":{"domain":"Ops.KCMeatUp.com","listen":"127.0.0.1:9443"}})",
      "/", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_EQ(cfg->operations_domain, "ops.kcmeatup.com");
  EXPECT_EQ(cfg->panel_domain(), "ops.kcmeatup.com");
  EXPECT_EQ(cfg->operations_listen_address, "127.0.0.1");
  EXPECT_EQ(cfg->operations_listen_port, 9443);
  EXPECT_FALSE(parse_config(R"({"domains":{"kcmeatup.com":{"tenant":"kc"}},"operations":{"domain":"kcmeatup.com"}})",
                            "/", &err));
  EXPECT_NE(err.find("tenant's domain"), std::string::npos) << err;
  EXPECT_FALSE(parse_config(R"({"operations":{"domain":"not a name"}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"operations":{"listen":"9443"}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"operations":{"listen":"127.0.0.1:99999"}})", "/", &err));
  auto none = parse_config("{}", "/", &err);
  ASSERT_TRUE(none) << err;
  EXPECT_EQ(none->panel_domain(), "");
  EXPECT_EQ(goober::config::local_mode(none)->panel_domain(), "ops.localhost");
  EXPECT_FALSE(none->operations_listen_port);
}
