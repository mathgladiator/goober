#include "goober/web/robots.h"

#include <gtest/gtest.h>

#include <chrono>

using namespace goober::web;

TEST(Robots, MatchesPatterns) {
  EXPECT_TRUE(robots_match("/shop", "/shop/beef"));
  EXPECT_FALSE(robots_match("/shop", "/sho"));
  EXPECT_TRUE(robots_match("/*.pdf$", "/files/price.pdf"));
  EXPECT_FALSE(robots_match("/*.pdf$", "/files/price.pdf?x=1"));
  EXPECT_TRUE(robots_match("/*/private", "/a/b/private/x"));
  EXPECT_TRUE(robots_match("*", "/anything"));
  EXPECT_TRUE(robots_match("/$", "/"));
  EXPECT_FALSE(robots_match("/$", "/x"));
}

TEST(Robots, ReadsOurGroupOrTheStarGroup) {
  auto text = R"(
# a comment
User-agent: *
Disallow: /cart
Allow: /cart/public
Crawl-delay: 2.5

User-agent: Googlebot
User-agent: Bingbot
Disallow: /

Sitemap: https://ranch.example/sitemap.xml
)";
  auto r = Robots::parse(text, "goober");
  EXPECT_FALSE(r.allowed("/cart/checkout"));
  EXPECT_TRUE(r.allowed("/cart/public/list"));  // the longer pattern wins
  EXPECT_TRUE(r.allowed("/shop/beef"));
  EXPECT_TRUE(r.allowed("/robots.txt"));
  EXPECT_EQ(r.crawl_delay_ms(), 2500);
  ASSERT_EQ(r.sitemaps().size(), 1u);
  EXPECT_EQ(r.sitemaps()[0], "https://ranch.example/sitemap.xml");

  auto mine = Robots::parse("User-agent: *\nDisallow: /\n\nuser-agent: GOOBER\nDisallow: /admin\n", "goober");
  EXPECT_TRUE(mine.allowed("/shop"));  // our own group replaces *, it doesn't add to it
  EXPECT_FALSE(mine.allowed("/admin/x"));

  EXPECT_TRUE(Robots::parse("User-agent: *\nDisallow:\n", "goober").allowed("/x"));  // empty disallows nothing
  EXPECT_FALSE(Robots::disallow_all().allowed("/x"));
  EXPECT_TRUE(Robots::allow_all().allowed("/x"));
  EXPECT_EQ(Robots::parse("User-agent: *\nCrawl-delay: 9999\n", "goober").crawl_delay_ms(), 60000);
}

TEST(Robots, HostileWildcardsCostABoundedAmount) {
  std::string text = "User-agent: *\n";
  std::string pattern = "/*" + std::string(500, 'a') + "b\n";
  for (int i = 0; i < 1000; ++i) text += "Disallow: " + pattern;
  auto r = Robots::parse(text, "goober");
  auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(r.allowed("/" + std::string(8000, 'a')));  // the budget runs out: don't fetch
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}
