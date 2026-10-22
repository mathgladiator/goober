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
