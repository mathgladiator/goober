#include <gtest/gtest.h>

#include <thread>

#include "goober/content/content.h"

using namespace goober;
namespace fs = std::filesystem;

class ContentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "content";
    fs::remove_all(root);
    w = std::make_unique<wal::HostWal>(root);
    store = std::make_unique<content::ContentStore>(*w);
  }
  fs::path root;
  std::unique_ptr<wal::HostWal> w;
  std::unique_ptr<content::ContentStore> store;
};

TEST_F(ContentTest, PutGetListRemove) {
  std::string err;
  ASSERT_TRUE(store->put("playground", "index.html", "<h1>hi</h1>", &err)) << err;
  ASSERT_TRUE(store->put("playground", "css/site.css", "body{}", &err)) << err;
  auto f = store->get("playground", "index.html");
  ASSERT_TRUE(f);
  EXPECT_EQ(f->data, "<h1>hi</h1>");
  EXPECT_EQ(f->content_type, "text/html; charset=utf-8");
  EXPECT_EQ(f->etag.size(), 18u);
  EXPECT_EQ(store->list("playground", ""), (std::vector<std::string>{"css/site.css", "index.html"}));
  EXPECT_EQ(store->list("playground", "css/"), (std::vector<std::string>{"css/site.css"}));
  EXPECT_TRUE(fs::exists(root / "content/playground/css/site.css"));
  ASSERT_TRUE(store->remove("playground", "css/site.css", &err));
  EXPECT_FALSE(store->get("playground", "css/site.css"));
  EXPECT_FALSE(store->remove("playground", "css/site.css", &err));
}

TEST_F(ContentTest, ResolveUrlPaths) {
  std::string err;
  store->put("t", "index.html", "root", &err);
  store->put("t", "docs/index.html", "docs", &err);
  store->put("t", "about", "about", &err);
  EXPECT_EQ(store->resolve("t", "/")->data, "root");
  EXPECT_EQ(store->resolve("t", "/docs")->data, "docs");
  EXPECT_EQ(store->resolve("t", "/about")->data, "about");
  EXPECT_FALSE(store->resolve("t", "/nope"));
}

TEST_F(ContentTest, RefusesBadTreesAndPaths) {
  std::string err;
  EXPECT_FALSE(store->put("Bad", "x.html", "", &err));
  EXPECT_FALSE(store->put("t", "../x.html", "", &err));
  EXPECT_FALSE(store->put("t", "/x.html", "", &err));
  EXPECT_TRUE(store->put(content::kHostTree, "error-message/404.html", "nope", &err));
  EXPECT_FALSE(store->get("t", "../../wal/host.wal"));
}

TEST(ContentType, ByExtension) {
  EXPECT_EQ(content::content_type_for("a/b.PNG"), "image/png");
  EXPECT_EQ(content::content_type_for("noext"), "application/octet-stream");
  EXPECT_EQ(content::content_type_for("dir.d/noext"), "application/octet-stream");
}

TEST(ContentType, DataAndFeedFiles) {
  EXPECT_EQ(content::content_type_for("calculate/meat-macros.csv"), "text/csv; charset=utf-8");
  EXPECT_EQ(content::content_type_for("updates.atom"), "application/atom+xml; charset=utf-8");
  EXPECT_EQ(content::content_type_for("site.webmanifest"), "application/manifest+json");
  EXPECT_EQ(content::content_type_for("x.json"), "application/json");
}

TEST_F(ContentTest, APreviewIsSeenOnlyByItsOwnThreadAndScope) {
  std::string err;
  ASSERT_TRUE(store->put("t", "a.md", "old", &err));
  ASSERT_TRUE(store->put("t", "gone.md", "x", &err));
  content::Overlay o{"t", {{"a.md", "new"}, {"b/new.md", "fresh"}, {"gone.md", std::nullopt}}};
  {
    content::ContentStore::Preview preview(&o);
    EXPECT_EQ(store->get("t", "a.md")->data, "new");
    EXPECT_EQ(store->get("t", "b/new.md")->data, "fresh");
    EXPECT_FALSE(store->get("t", "gone.md"));
    EXPECT_EQ(store->list("t", ""), (std::vector<std::string>{"a.md", "b/new.md"}));
    EXPECT_EQ(store->list("other", ""), std::vector<std::string>{});
    std::optional<content::File> elsewhere;
    std::thread([&] { elsewhere = store->get("t", "a.md"); }).join();
    EXPECT_EQ(elsewhere->data, "old");
  }
  EXPECT_EQ(store->get("t", "a.md")->data, "old");
  EXPECT_FALSE(store->get("t", "b/new.md"));
  EXPECT_TRUE(store->get("t", "gone.md"));
  EXPECT_FALSE(fs::exists(root / "content/t/b/new.md"));
}
