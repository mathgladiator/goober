#include <gtest/gtest.h>

#include "goober/router/path.h"
#include "goober/router/table.h"

using namespace goober::router;

TEST(Normalize, CollapsesTrimsDecodes) {
  EXPECT_EQ(normalize_path("/")->path, "/");
  EXPECT_EQ(normalize_path("//")->path, "/");
  EXPECT_EQ(normalize_path("/a//b/")->path, "/a/b");
  EXPECT_EQ(normalize_path("/a%20b")->segments[0], "a b");
  EXPECT_FALSE(normalize_path("a/b"));
  EXPECT_FALSE(normalize_path("/a/../b"));
  EXPECT_FALSE(normalize_path("/a/%2e%2e/b"));
  EXPECT_FALSE(normalize_path("/a%2Fb"));
  EXPECT_FALSE(normalize_path("/a%00"));
  EXPECT_FALSE(normalize_path("/a%zz"));
  EXPECT_FALSE(normalize_path("/./a"));
}

TEST(Query, Parses) {
  auto q = parse_query("a=1&b=x%20y&c&d=e+f");
  EXPECT_EQ(q["a"], "1");
  EXPECT_EQ(q["b"], "x y");
  EXPECT_EQ(q["c"], "");
  EXPECT_EQ(q["d"], "e f");
  EXPECT_TRUE(parse_query("").empty());
}

TEST(Pattern, ParsesAndRefuses) {
  std::string err;
  auto p = parse_pattern("/hello/{name : string}/{id:int64}", &err);
  ASSERT_TRUE(p) << err;
  EXPECT_EQ(p->segments.size(), 3u);
  EXPECT_EQ(p->shape(), "/hello/{}/{}");
  EXPECT_FALSE(parse_pattern("hello", &err));
  EXPECT_FALSE(parse_pattern("/a//b", &err));
  EXPECT_FALSE(parse_pattern("/a/", &err));
  EXPECT_FALSE(parse_pattern("/{x : float}", &err));
  EXPECT_FALSE(parse_pattern("/{x}{y}", &err));
  EXPECT_FALSE(parse_pattern("/{x}/{x}", &err));
  EXPECT_FALSE(parse_pattern("/a b", &err));
  EXPECT_TRUE(parse_pattern("/", &err));
}

TEST(Types, IntegersFit) {
  EXPECT_TRUE(matches_type("42", ParamType::kInt64));
  EXPECT_TRUE(matches_type("-42", ParamType::kInt64));
  EXPECT_FALSE(matches_type("4x", ParamType::kInt64));
  EXPECT_FALSE(matches_type("-", ParamType::kInt64));
  EXPECT_FALSE(matches_type("99999999999999999999", ParamType::kInt64));
  EXPECT_FALSE(matches_type("3000000000", ParamType::kInt32));
  EXPECT_TRUE(matches_type("anything", ParamType::kString));
}

TEST(Table, LiteralBeatsParameter) {
  std::string err;
  RouteTable<std::string> t;
  ASSERT_TRUE(t.add(*parse_pattern("/jobs/{slug : string}", &err), "get", "slug"));
  ASSERT_TRUE(t.add(*parse_pattern("/jobs/new", &err), "get", "new"));
  ASSERT_TRUE(t.add(*parse_pattern("/jobs/{id : int64}/notes", &err), "get", "notes"));
  auto m = t.match({"jobs", "new"}, "get");
  ASSERT_TRUE(m.value);
  EXPECT_EQ(*m.value, "new");
  m = t.match({"jobs", "plumbing"}, "get");
  EXPECT_EQ(*m.value, "slug");
  EXPECT_EQ(m.params["slug"], "plumbing");
  m = t.match({"jobs", "7", "notes"}, "get");
  EXPECT_EQ(*m.value, "notes");
  EXPECT_EQ(m.params["id"], "7");
  m = t.match({"jobs", "x", "notes"}, "get");
  EXPECT_FALSE(m.value);
  m = t.match({"jobs", "new"}, "post");
  EXPECT_FALSE(m.value);
  EXPECT_TRUE(m.method_not_allowed);
  // What the match chose among: both /jobs routes fit /jobs/new.
  auto fits = t.fitting({"jobs", "new"});
  ASSERT_EQ(fits.size(), 2u);
  EXPECT_EQ(*fits[0].second, "slug");
  EXPECT_EQ(*fits[1].second, "new");
  EXPECT_TRUE(t.fitting({"nope"}).empty());
}

TEST(Table, ConflictsOnShape) {
  std::string err;
  RouteTable<int> t;
  ASSERT_TRUE(t.add(*parse_pattern("/a/{x : string}", &err), "get", 1));
  EXPECT_FALSE(t.add(*parse_pattern("/a/{y : int64}", &err), "get", 2));
  EXPECT_TRUE(t.add(*parse_pattern("/a/{y : int64}", &err), "post", 3));
}
