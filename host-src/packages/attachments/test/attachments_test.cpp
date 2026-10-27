#include <gtest/gtest.h>

#include "goober/attachments/attachments.h"

using namespace goober;
namespace fs = std::filesystem;

namespace {
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("rest-of-png", 11);
}

TEST(Sniff, DetectsFromBytesOnly) {
  EXPECT_EQ(attachments::sniff(kPng)->ext, "png");
  EXPECT_EQ(attachments::sniff("\xFF\xD8\xFF\xE0xxxx")->content_type, "image/jpeg");
  EXPECT_EQ(attachments::sniff("GIF89a....")->ext, "gif");
  EXPECT_EQ(attachments::sniff(std::string("RIFF\0\0\0\0WEBPVP8 ", 16))->ext, "webp");
  EXPECT_EQ(attachments::sniff("%PDF-1.7")->inline_ok, false);
  auto svg = attachments::sniff("  <?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
  ASSERT_TRUE(svg);
  EXPECT_EQ(svg->ext, "svg");
  EXPECT_FALSE(svg->inline_ok);
  EXPECT_FALSE(attachments::sniff("<html><script>alert(1)</script>"));
  EXPECT_FALSE(attachments::sniff("plain text"));
}

TEST(Attachments, PutGetListRemoveAndNeverReuseIds) {
  fs::path root = fs::path(GOOBER_TEST_TMP) / "attachments";
  fs::remove_all(root);
  wal::HostWal w(root);
  attachments::Store store(w);
  std::string err;
  auto a = store.put("t", kPng, "cat.png", "a cat", "", 1000, &err);
  ASSERT_TRUE(a) << err;
  EXPECT_EQ(a->id, 1);
  EXPECT_EQ(a->url(), "/attachment/1.png");
  EXPECT_EQ(a->visibility, "public");
  EXPECT_EQ(*store.read("t", 1), kPng);
  auto b = store.put("t", "GIF89a....", "x", "", "members", 2000, &err);
  ASSERT_TRUE(b) << err;
  EXPECT_EQ(store.list("t").size(), 2u);
  EXPECT_EQ(store.list("t")[0].id, 2);
  EXPECT_FALSE(store.put("t", "<html>", "x.png", "", "", 3000, &err));
  EXPECT_FALSE(store.put("t", kPng, "x", "", "secret", 3000, &err));
  ASSERT_TRUE(store.remove("t", 2, 4000, &err)) << err;
  EXPECT_FALSE(store.get("t", 2));
  EXPECT_FALSE(store.read("t", 2));
  auto c = store.put("t", kPng, "again", "", "", 5000, &err);
  EXPECT_EQ(c->id, 3);
  EXPECT_FALSE(store.get("other", 1));
}

TEST(Attachments, ReferencesComeFromTextContent) {
  fs::path root = fs::path(GOOBER_TEST_TMP) / "attachment-refs";
  fs::remove_all(root);
  wal::HostWal w(root);
  content::ContentStore content(w);
  std::string err;
  ASSERT_TRUE(content.put("t", "index.md", "![a](/attachment/1.png) and /attachment/1.png", &err));
  ASSERT_TRUE(content.put("t", "css/site.css", "body{background:url(/attachment/12.jpg)}", &err));
  ASSERT_TRUE(content.put("t", "logo.png", "/attachment/99.png", &err));  // binary files aren't scanned
  auto refs = attachments::references(content, "t");
  EXPECT_EQ(refs.size(), 2u);
  EXPECT_EQ(refs[1], (std::vector<std::string>{"index.md"}));
  EXPECT_EQ(refs[12], (std::vector<std::string>{"css/site.css"}));
}
