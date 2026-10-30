// The operations audit (design/machine/028.Panel.md § 8): what a deploy suggests about how the
// host is run, with a made-up resolver so DNS is the test's to decide.

#include <gtest/gtest.h>

#include <map>

#include "goober/host/audit.h"

using namespace goober;

namespace {

std::shared_ptr<const config::HostConfig> parse(const std::string& text) {
  std::string err;
  auto cfg = config::parse_config(text, "/tmp", &err);
  EXPECT_TRUE(cfg) << err;
  return cfg;
}

host::Resolver dns(std::map<std::string, std::vector<std::string>> table) {
  return [table](const std::string& name) {
    auto it = table.find(name);
    return it == table.end() ? std::vector<std::string>{} : it->second;
  };
}

const host::AuditFinding& find(const std::vector<host::AuditFinding>& all, const std::string& id) {
  for (const auto& f : all) {
    if (f.id == id) return f;
  }
  static host::AuditFinding none;
  ADD_FAILURE() << "no finding " << id;
  return none;
}

const char* kCaddy = R"(# written by setup
{
  email ops@shop.example
}
(common) {
  encode gzip
}
shop.example, https://blog.example:443 {
  reverse_proxy 127.0.0.1:8080
}
)";

const std::string kBase = R"("platform-domain": "shop.example",
  "domains": {"shop.example": {"tenant": "shop"}, "blog.example": {"tenant": "blog"}},
  "identity": {"mailer": "ses", "ses": {"from": "no-reply@shop.example"}, "operators": {"me@shop.example": ["host:owner"]}})";

}  // namespace

TEST(Audit, CaddySitesAreTheSiteBlocksAddresses) {
  EXPECT_EQ(host::caddy_sites(kCaddy), (std::vector<std::string>{"shop.example", "blog.example"}));
}

TEST(Audit, SuggestsAPanelAndSaysWhenItsDnsIsReady) {
  auto cfg = parse("{" + kBase + "}");
  auto all = host::audit_operations(*cfg, kCaddy, dns({{"shop.example", {"203.0.113.7"}}}));
  const auto& p = find(all, "panel");
  EXPECT_EQ(p.level, "suggest");
  EXPECT_EQ(p.data.at("domain"), "ops.shop.example");
  EXPECT_EQ(p.data.at("dns"), "missing");
  ASSERT_FALSE(p.steps.empty());
  EXPECT_NE(p.steps[0].find("A ops.shop.example -> 203.0.113.7"), std::string::npos) << p.steps[0];
  EXPECT_EQ(p.data.at("alternatives").at(0), "ops.blog.example");

  // Once ops. resolves here, the deploy can set it up: no DNS step.
  all = host::audit_operations(*cfg, kCaddy, dns({{"shop.example", {"203.0.113.7"}}, {"ops.shop.example", {"203.0.113.7"}}}));
  EXPECT_EQ(find(all, "panel").data.at("dns"), "ready");
  EXPECT_EQ(find(all, "panel").steps[0].find("Add DNS"), std::string::npos);
  all = host::audit_operations(*cfg, kCaddy, dns({{"shop.example", {"203.0.113.7"}}, {"ops.shop.example", {"198.51.100.1"}}}));
  EXPECT_EQ(find(all, "panel").data.at("dns"), "elsewhere");
}

TEST(Audit, QuietChecksAreNotedNotSuggested) {
  auto cfg = parse("{" + kBase + R"(, "audit": {"quiet": ["panel", "backup", "nonsense"]}})");
  auto all = host::audit_operations(*cfg, kCaddy, dns({}));
  EXPECT_EQ(find(all, "panel").level, "quiet");
  EXPECT_TRUE(find(all, "panel").steps.empty());
  EXPECT_NE(find(all, "panel").title.find("audit.quiet"), std::string::npos);
  EXPECT_EQ(find(all, "backup").level, "quiet");
  EXPECT_EQ(find(all, "audit").level, "warn");  // "nonsense" is no check's id
  std::string err;
  EXPECT_FALSE(config::parse_config(R"({"audit": {"quiet": ["Panel!"]}})", "/tmp", &err));
  EXPECT_FALSE(config::parse_config(R"({"audit": {"mute": []}})", "/tmp", &err));
}

TEST(Audit, AConfiguredPanelIsCheckedForDnsAndCaddy) {
  auto cfg = parse("{" + kBase + R"(, "operations": {"domain": "ops.shop.example"}})");
  auto here = dns({{"shop.example", {"203.0.113.7"}}, {"ops.shop.example", {"203.0.113.7"}}});
  auto all = host::audit_operations(*cfg, kCaddy, here);
  EXPECT_EQ(find(all, "panel").level, "ok");
  EXPECT_EQ(find(all, "caddy").level, "warn");  // the panel's domain isn't in the Caddyfile
  EXPECT_NE(find(all, "caddy").title.find("ops.shop.example"), std::string::npos);
  std::string caddy = kCaddy;
  caddy.replace(caddy.find("shop.example, "), 14, "shop.example, ops.shop.example, ");
  all = host::audit_operations(*cfg, caddy, here);
  EXPECT_EQ(find(all, "caddy").level, "ok");
  // A wildcard site covers it too.
  all = host::audit_operations(*cfg, std::string(kCaddy) + "*.shop.example {\n  tls internal\n}\n", here);
  EXPECT_EQ(find(all, "caddy").level, "ok");
  all = host::audit_operations(*cfg, caddy, dns({{"shop.example", {"203.0.113.7"}}}));
  EXPECT_EQ(find(all, "panel").level, "warn");
}

TEST(Audit, MailBackupsAndOperators) {
  auto cfg = parse(R"({"platform-domain": "shop.example", "domains": {"shop.example": {"tenant": "shop"}}})");
  auto all = host::audit_operations(*cfg, "", dns({}));
  EXPECT_EQ(find(all, "mailer").level, "warn");
  EXPECT_EQ(find(all, "backup").level, "suggest");
  EXPECT_EQ(find(all, "operators").level, "warn");
  for (const auto& f : all) EXPECT_NE(f.id, "caddy");  // no Caddyfile given: not checked
  cfg = parse("{" + kBase + R"(, "backup": {"s3": {"bucket": "shop-backups"}}})");
  all = host::audit_operations(*cfg, kCaddy, dns({}));
  EXPECT_EQ(find(all, "mailer").level, "ok");
  EXPECT_EQ(find(all, "backup").level, "ok");
  EXPECT_EQ(find(all, "operators").level, "ok");
  EXPECT_EQ(find(all, "caddy").level, "ok");
}
