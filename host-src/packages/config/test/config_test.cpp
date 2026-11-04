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

// Disk budgets (045.Limits.md): defaults for a small server, each overridable, each floored.
TEST(Config, DiskLimitsHaveDefaultsAndBounds) {
  std::string err;
  auto plain = parse_config(R"({"operations":{"token":"0123456789abcdef"}})", "/", &err);
  ASSERT_TRUE(plain) << err;
  EXPECT_EQ(plain->disk.try_bytes, int64_t(512) << 20);
  EXPECT_EQ(plain->disk.traffic_bytes, int64_t(64) << 20);
  EXPECT_EQ(plain->disk.tmp_hours, 24);
  auto set = parse_config(
      R"({"operations":{"token":"0123456789abcdef"},"limits":{"try-bytes":2147483648,"snapshot-files":20,"tmp-hours":6}})", "/",
      &err);
  ASSERT_TRUE(set) << err;
  EXPECT_EQ(set->disk.try_bytes, int64_t(2) << 30);
  EXPECT_EQ(set->disk.snapshot_files, 20);
  EXPECT_EQ(set->disk.tmp_hours, 6);
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"traffic-bytes":1000}})", "/", &err));
  EXPECT_NE(err.find("limits.traffic-bytes"), std::string::npos) << err;
  err.clear();
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"try-bytes":"big"}})", "/", &err));
  // Box versions: archived after 30 days inactive, deleted after 400, the newest 3 kept.
  EXPECT_EQ(plain->disk.version_archive_days, 30);
  EXPECT_EQ(plain->disk.version_delete_days, 400);
  EXPECT_EQ(plain->disk.version_keep, 3);
  err.clear();
  EXPECT_FALSE(parse_config(
      R"({"operations":{"token":"0123456789abcdef"},"limits":{"version-archive-days":500,"version-delete-days":400}})", "/", &err));
  EXPECT_NE(err.find("version-delete-days must be more"), std::string::npos) << err;
  err.clear();
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"version-delete-days":30}})", "/", &err));
  err.clear();
  // The daily upload budget (019 § 9): 1 GiB unless set; 0 turns it off.
  EXPECT_EQ(plain->disk.upload_bytes_per_day, int64_t(1) << 30);
  auto off = parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"upload-bytes-per-day":0}})", "/", &err);
  ASSERT_TRUE(off) << err;
  EXPECT_EQ(off->disk.upload_bytes_per_day, 0);
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"upload-bytes-per-day":-1}})", "/", &err));
  // Heat study reports (048): 64 MiB unless set, at least 1 MiB.
  EXPECT_EQ(plain->disk.studies_bytes, int64_t(64) << 20);
  auto studies = parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"studies-bytes":2097152}})", "/", &err);
  ASSERT_TRUE(studies) << err;
  EXPECT_EQ(studies->disk.studies_bytes, 2097152);
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"studies-bytes":1000}})", "/", &err));
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
      R"({"domains":{"acme.example":{"tenant":"acme"}},"operations":{"domain":"Ops.Acme.Example","listen":"127.0.0.1:9443"}})",
      "/", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_EQ(cfg->operations_domain, "ops.acme.example");
  EXPECT_EQ(cfg->panel_domain(), "ops.acme.example");
  EXPECT_EQ(cfg->operations_listen_address, "127.0.0.1");
  EXPECT_EQ(cfg->operations_listen_port, 9443);
  EXPECT_FALSE(parse_config(R"({"domains":{"acme.example":{"tenant":"acme"}},"operations":{"domain":"acme.example"}})",
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

TEST(Config, PwaPushHostsAndContact) {
  std::string err;
  auto cfg = parse_config(R"({"pwa":{"push-hosts":["push.example.com","*.push.example.net"],"push-contact":"mailto:ops@example.com"}})",
                          "/", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_EQ(cfg->pwa.push_hosts.size(), 2u);
  EXPECT_EQ(cfg->pwa.push_contact, "mailto:ops@example.com");
  EXPECT_FALSE(parse_config(R"({"pwa":{"push-hosts":["not a host"]}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"pwa":{"push-contact":"ops@example.com"}})", "/", &err));
}

TEST(Config, TenantsCarryPostalAddressTermsAndInviteRoles) {
  std::string err;
  auto cfg = parse_config(R"({"domains":{"acme.example.com":{"tenant":"acme"}},
      "mail":{"outreach-daily-max":20},
      "tenants":{"acme":{"postal-address":"Acme Grill, 1 Main St, Springfield, IL 62701","terms":"/terms","privacy":"/privacy",
                       "invites":{"roles":["member","editor/blog"],"daily-max":30,"per-inviter-daily":5}}}})",
                          "/", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_EQ(cfg->tenant("acme").postal_address, "Acme Grill, 1 Main St, Springfield, IL 62701");
  EXPECT_EQ(cfg->tenant("acme").terms, "/terms");
  EXPECT_EQ(cfg->tenant("acme").invites.roles.size(), 2u);
  EXPECT_EQ(cfg->tenant("acme").invites.daily_max, 30);
  EXPECT_EQ(cfg->mail.outreach_daily_max, 20);
  EXPECT_EQ(cfg->tenant("other").invites.daily_max, 50);  // defaults for a tenant without an entry
  EXPECT_TRUE(cfg->tenant("other").invites.roles.empty());
  EXPECT_FALSE(parse_config(R"({"tenants":{"acme":{}}})", "/", &err));  // no domain serves it
  EXPECT_FALSE(parse_config(R"({"domains":{"a.com":{"tenant":"acme"}},"tenants":{"acme":{"terms":"terms"}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"domains":{"a.com":{"tenant":"acme"}},"tenants":{"acme":{"invites":{"roles":["host:owner"]}}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"domains":{"a.com":{"tenant":"acme"}},"tenants":{"acme":{"postal-address":"a\nb"}}})", "/", &err));
  EXPECT_FALSE(parse_config(R"({"domains":{"a.com":{"tenant":"acme"}},"tenants":{"acme":{"colour":"red"}}})", "/", &err));
}

TEST(Config, RedirectDomainsSendEverythingToTheirPrimary) {
  std::string err;
  auto cfg = parse_config(
      R"({"domains":{"example.com":{"tenant":"shop"},"WWW.Example.com":{"redirect-to":"Example.com"}}})", "/", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_EQ(cfg->domains.size(), 1u);  // a redirect is no tenant's domain
  EXPECT_EQ(*cfg->redirect_for("www.example.com:443"), "example.com");
  EXPECT_EQ(*cfg->redirect_for("WWW.example.com."), "example.com");
  EXPECT_FALSE(cfg->redirect_for("example.com"));
  EXPECT_FALSE(cfg->tenant_for_host("www.example.com"));
  EXPECT_FALSE(cfg->redirect_for("www.example.com.localhost"));
  auto local = local_mode(cfg);
  EXPECT_EQ(*local->redirect_for("www.example.com.localhost:8080"), "example.com.localhost");

  auto refused = [&](const char* text, const char* says) {
    err.clear();
    EXPECT_FALSE(parse_config(text, "/", &err)) << text;
    EXPECT_NE(err.find(says), std::string::npos) << err;
  };
  // Only to a tenant's domain: not to nothing, not to another redirect.
  refused(R"({"domains":{"www.example.com":{"redirect-to":"example.com"}}})", "isn't a tenant's domain");
  refused(R"({"domains":{"example.com":{"tenant":"shop"},"a.example.com":{"redirect-to":"example.com"},
              "b.example.com":{"redirect-to":"a.example.com"}}})", "isn't a tenant's domain");
  refused(R"({"domains":{"example.com":{"tenant":"shop"},"www.example.com":{"redirect-to":"example.com","tenant":"shop"}}})",
          "takes nothing else");
  refused(R"({"domains":{"example.com":{"tenant":"shop"},"www.example.com":{"redirect-to":5}}})", "redirect-to");
  refused(R"({"domains":{"example.com":{"tenant":"shop"},"www":{"redirect-to":"example.com"}}})", "DNS name");
  refused(R"({"domains":{"example.com":{"tenant":"shop"},"www.example.com":{"tenant":"shop"},
              "WWW.example.com":{"redirect-to":"example.com"}}})", "listed twice");
  refused(R"({"domains":{"example.com":{"tenant":"shop"},"ops.example.com":{"redirect-to":"example.com"}},
              "operations":{"domain":"ops.example.com"}})", "redirect domain");
}

// Behind a proxy the host believes X-Forwarded-For, so only loopback may reach it (043).
TEST(Config, BehindAProxyListensOnLoopbackOnly) {
  std::string err;
  auto with = [&](const std::string& address) {
    err.clear();
    return parse_config(R"({"operations":{"token":"0123456789abcdef"},"listen":{"address":")" + address +
                            R"(","behind-proxy":true}})",
                        "/", &err);
  };
  EXPECT_TRUE(with("127.0.0.1")) << err;
  EXPECT_TRUE(with("127.0.1.1")) << err;
  EXPECT_TRUE(with("::1")) << err;
  for (const char* bad : {"0.0.0.0", "10.0.0.5", "::", "localhost", "127.0.0", "127.0.0.256", "127.0.0.1x"}) {
    EXPECT_FALSE(with(bad)) << bad;
    EXPECT_NE(err.find("listen.behind-proxy is on"), std::string::npos) << err;
    EXPECT_NE(err.find("127.0.0.1"), std::string::npos) << err;
  }
  // Without a proxy any address is the operator's choice.
  err.clear();
  EXPECT_TRUE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"listen":{"address":"0.0.0.0"}})", "/", &err)) << err;
  EXPECT_TRUE(is_loopback_address("127.0.0.1"));
  EXPECT_FALSE(is_loopback_address("128.0.0.1"));
}

// Per-domain controls (047 § 2): host.json holds only their host defaults, checked strictly and
// only lower than the host's own limits.
TEST(Config, ControlDefaultsAreCheckedAgainstTheHostsLimits) {
  std::string err;
  auto ok = parse_config(R"({"operations":{"token":"0123456789abcdef"},
      "controls":{"defaults":{"rate":{"per-minute":600},"mode":"observe"}}})", "/", &err);
  ASSERT_TRUE(ok) << err;
  ASSERT_TRUE(ok->controls_defaults);
  EXPECT_EQ(ok->controls_defaults->mode, goober::controls::Mode::kObserve);
  EXPECT_EQ(ok->controls_defaults->props.rate.value.per_minute, 600);
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"controls":{"domains":{}}})", "/", &err));
  EXPECT_NE(err.find("controls"), std::string::npos) << err;
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"body-max-bytes":1000},
      "controls":{"defaults":{"body-max-bytes":2000}}})", "/", &err));
  EXPECT_NE(err.find("may only lower it"), std::string::npos) << err;
  EXPECT_FALSE(parse_config(
      R"({"operations":{"token":"0123456789abcdef"},"controls":{"defaults":{"rate":{"per-minute":1,"bursty":2}}}})", "/", &err));
  EXPECT_NE(err.find("unknown key bursty"), std::string::npos) << err;
  // A default fuel is taken up to limits.fuel's max, and refused past it.
  auto fuel = parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"fuel":{"default":1000000,"max":5000000}},
      "controls":{"defaults":{"fuel":2000000}}})", "/", &err);
  ASSERT_TRUE(fuel) << err;
  EXPECT_EQ(fuel->controls_defaults->props.fuel.value, 2000000);
  EXPECT_FALSE(parse_config(R"({"operations":{"token":"0123456789abcdef"},"limits":{"fuel":{"default":1000000,"max":5000000}},
      "controls":{"defaults":{"fuel":6000000}}})", "/", &err));
  EXPECT_NE(err.find("limits.fuel max"), std::string::npos) << err;
}
