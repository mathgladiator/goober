#include <gtest/gtest.h>

#include "goober/render/frontmatter.h"

using namespace goober::render;

TEST(FrontMatter, SplitsFieldsFromBody) {
  std::string err;
  auto doc = split_front_matter(
      "---\ntitle: Hello: world\ndate: 2026-10-26\ndraft: false\norder: 3\nquoted: \"  padded \"\n"
      "requirements: [[\"owner\"]]\ntags:\n  - news\n  - goober\n# a comment\n---\n# Body\n",
      &err);
  EXPECT_TRUE(err.empty()) << err;
  ASSERT_TRUE(doc.has_front_matter);
  EXPECT_EQ(doc.fields.at("title"), "Hello: world");
  EXPECT_EQ(doc.fields.at("date"), "2026-10-26");
  EXPECT_EQ(doc.fields.at("draft"), false);
  EXPECT_EQ(doc.fields.at("order"), 3);
  EXPECT_EQ(doc.fields.at("quoted"), "  padded ");
  EXPECT_EQ(doc.fields.at("requirements").at(0).at(0), "owner");
  EXPECT_EQ(doc.fields.at("tags").as_array().size(), 2u);
  EXPECT_EQ(doc.body, "# Body\n");
  auto escaped = split_front_matter("---\ntitle: \"Say \\\"hi\\\"\\nthere\"\n---\n");
  EXPECT_EQ(escaped.fields.at("title"), "Say \"hi\"\nthere");
}

TEST(FrontMatter, NoneOrUnclosedIsAllBody) {
  auto none = split_front_matter("# Just a page\n");
  EXPECT_FALSE(none.has_front_matter);
  EXPECT_EQ(none.body, "# Just a page\n");
  auto unclosed = split_front_matter("---\ntitle: x\nno end");
  EXPECT_FALSE(unclosed.has_front_matter);
  EXPECT_EQ(unclosed.body, "---\ntitle: x\nno end");
  auto dots = split_front_matter("---\r\ntitle: x\r\n...\r\nbody");
  EXPECT_TRUE(dots.has_front_matter);
  EXPECT_EQ(dots.fields.at("title"), "x");
  EXPECT_EQ(dots.body, "body");
}

TEST(FrontMatter, BadLinesAreReportedAndSkipped) {
  std::string err;
  auto doc = split_front_matter("---\nnot a field\nbad key!: 1\nlist: [oops\nok: yes\n---\n", &err);
  EXPECT_FALSE(err.empty());
  EXPECT_EQ(doc.fields.size(), 1u);
  EXPECT_EQ(doc.fields.at("ok"), "yes");
}
