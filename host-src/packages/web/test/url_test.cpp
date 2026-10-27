#include "goober/web/url.h"

#include <gtest/gtest.h>

using namespace goober::web;

TEST(Url, ParsesAndNormalizes) {
  auto u = parse_url("  HTTPS://Example.COM:443/a/./b/../c?x=1#frag ");
  ASSERT_TRUE(u);
  EXPECT_EQ(u->str(), "https://example.com/a/c?x=1");
  EXPECT_EQ(u->origin(), "https://example.com");
  EXPECT_EQ(u->effective_port(), 443);
  EXPECT_EQ(parse_url("http://example.com")->str(), "http://example.com/");
  EXPECT_EQ(parse_url("http://example.com:8080/x")->str(), "http://example.com:8080/x");
  EXPECT_EQ(parse_url("http://[::1]:8080/")->host, "[::1]");
  EXPECT_EQ(parse_url("https://example.com/a b/ü")->path, "/a%20b/%C3%BC");
  EXPECT_EQ(parse_url("https://example.com./x")->host, "example.com");
  EXPECT_EQ(parse_url("https://example.com/a/b/")->path, "/a/b/");
  EXPECT_EQ(parse_url("https://example.com/a/..")->path, "/");
  EXPECT_EQ(parse_url("https://example.com/../../x")->path, "/x");
}

TEST(Url, RefusesWhatIsntAPlainWebUrl) {
  for (const char* bad : {"ftp://example.com/", "javascript:alert(1)", "http://user:pass@example.com/",
                          "http://example.com:99999/", "http://exa mple.com/", "http:///x", "http://example.com/\x01",
                          "http://-bad.com/", "example.com/x"}) {
    EXPECT_FALSE(parse_url(bad)) << bad;
  }
}

TEST(Url, ResolvesReferences) {
  auto base = *parse_url("https://ranch.example/shop/beef/index.html?page=2");
  EXPECT_EQ(resolve(base, "steak.html")->str(), "https://ranch.example/shop/beef/steak.html");
  EXPECT_EQ(resolve(base, "../pork/")->str(), "https://ranch.example/shop/pork/");
  EXPECT_EQ(resolve(base, "/about")->str(), "https://ranch.example/about");
  EXPECT_EQ(resolve(base, "//cdn.example/x.js")->str(), "https://cdn.example/x.js");
  EXPECT_EQ(resolve(base, "?page=3")->str(), "https://ranch.example/shop/beef/index.html?page=3");
  EXPECT_EQ(resolve(base, "#top")->str(), base.str());
  EXPECT_EQ(resolve(base, "http://other.example/")->str(), "http://other.example/");
  EXPECT_FALSE(resolve(base, "mailto:hi@ranch.example"));
  EXPECT_FALSE(resolve(base, "javascript:void(0)"));
  EXPECT_TRUE(same_site(*parse_url("https://www.ranch.example/"), *parse_url("http://ranch.example/x")));
  EXPECT_FALSE(same_site(*parse_url("https://ranch.example/"), *parse_url("https://other.example/")));
}
