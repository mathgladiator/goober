#include "goober/web/robots.h"

#include <gtest/gtest.h>

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
