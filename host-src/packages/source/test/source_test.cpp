#include <gtest/gtest.h>

#include <filesystem>

#include "goober/base/files.h"
#include "goober/base/process.h"
#include "goober/source/source.h"

namespace goober::source {
namespace {

namespace fs = std::filesystem;

std::unique_ptr<Store> fresh(const std::string& name) {
  auto file = fs::path(GOOBER_TEST_TMP) / ("source-" + name) / "source.db";
  fs::remove_all(file.parent_path());
  return std::make_unique<Store>(file);
}

TEST(Source, CommitsCheckoutsAndLog) {
  auto s = fresh("basic");
  std::string err;
  ASSERT_TRUE(s->create("ranches", "the meat index", 1, &err)) << err;
  EXPECT_FALSE(s->create("ranches", "again", 1, &err));
  EXPECT_FALSE(s->create("Bad Name", "", 1, &err));
  auto r1 = s->commit("ranches", 0, {{"src/lib.rs", "fn main() {}\n"}, {"manifest.json", "{}"}}, "start", "token:grok", 10, &err);
  ASSERT_TRUE(r1) << err;
  EXPECT_EQ(r1->rev, 1);
  EXPECT_FALSE(r1->rebased);
  auto r2 = s->commit("ranches", 1, {{"src/lib.rs", "fn main() { hi(); }\n"}, {"README.md", "# r"}}, "more", "token:grok", 20, &err);
  ASSERT_TRUE(r2) << err;
  EXPECT_EQ(r2->rev, 2);

  auto files = s->files("ranches", 0, &err);
  ASSERT_TRUE(files);
  ASSERT_EQ(files->size(), 3u);
  EXPECT_EQ((*files)[0].path, "README.md");
  EXPECT_EQ(s->read("ranches", 0, "src/lib.rs", &err), "fn main() { hi(); }\n");
  EXPECT_EQ(s->read("ranches", 1, "src/lib.rs", &err), "fn main() {}\n");  // old revisions stay
  EXPECT_FALSE(s->read("ranches", 1, "README.md", &err));
  EXPECT_FALSE(s->files("ranches", 9, &err));

  auto log = s->log("ranches", 10);
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0].rev, 2);
  EXPECT_EQ(log[0].author, "token:grok");
  EXPECT_EQ(log[0].changed, (std::vector<std::string>{"README.md", "src/lib.rs"}));
  EXPECT_EQ(s->get("ranches")->head, 2);

  // Deleting, and a commit that changes nothing.
  auto r3 = s->commit("ranches", 2, {{"README.md", std::nullopt}}, "drop readme", "me", 30, &err);
  ASSERT_TRUE(r3) << err;
  EXPECT_EQ(s->files("ranches", 0, &err)->size(), 2u);
  EXPECT_FALSE(s->commit("ranches", 3, {{"manifest.json", "{}"}}, "same", "me", 40, &err));
  EXPECT_NE(err.find("nothing changed"), std::string::npos) << err;
}

TEST(Source, OthersWorkOnOtherFilesConflictsAreRefused) {
  auto s = fresh("conflicts");
  std::string err;
  ASSERT_TRUE(s->create("site", "", 1, &err));
  ASSERT_TRUE(s->commit("site", 0, {{"a.md", "a"}, {"b.md", "b"}}, "start", "one", 1, &err));
  // Two agents start from revision 1.
  ASSERT_TRUE(s->commit("site", 1, {{"a.md", "a2"}}, "agent one", "one", 2, &err));
  auto other = s->commit("site", 1, {{"b.md", "b2"}}, "agent two", "two", 3, &err);
  ASSERT_TRUE(other) << err;
  EXPECT_TRUE(other->rebased);
  EXPECT_EQ(s->read("site", 0, "a.md", &err), "a2");  // both changes kept
  EXPECT_EQ(s->read("site", 0, "b.md", &err), "b2");
  std::vector<std::string> conflicts;
  EXPECT_FALSE(s->commit("site", 1, {{"a.md", "a3"}}, "stale", "two", 4, &err, &conflicts));
  EXPECT_EQ(conflicts, std::vector<std::string>{"a.md"});
  EXPECT_NE(err.find("Read them again"), std::string::npos) << err;
  EXPECT_FALSE(s->commit("site", 7, {{"a.md", "x"}}, "future", "two", 4, &err));
}

TEST(Source, RevertsAndBinaryFiles) {
  auto s = fresh("revert");
  std::string err;
  ASSERT_TRUE(s->create("p", "", 1, &err));
  std::string png("\x89PNG\r\n\x1a\n\0\0", 10);
  ASSERT_TRUE(s->commit("p", 0, {{"a.txt", "one\n"}, {"logo.png", png}}, "1", "me", 1, &err));
  ASSERT_TRUE(s->commit("p", 1, {{"a.txt", "two\n"}, {"b.txt", "new\n"}, {"logo.png", std::nullopt}}, "2", "me", 2, &err));
  auto back = s->revert("p", 1, "me", 3, &err);
  ASSERT_TRUE(back) << err;
  EXPECT_EQ(*back, 3);
  EXPECT_EQ(s->read("p", 0, "a.txt", &err), "one\n");
  EXPECT_EQ(s->read("p", 0, "logo.png", &err), png);
  EXPECT_FALSE(s->read("p", 0, "b.txt", &err));
  EXPECT_EQ(s->log("p", 1)[0].message, "Back to revision 1");
  EXPECT_FALSE(s->revert("p", 3, "me", 4, &err));  // already there
  EXPECT_FALSE(is_text(png));
  EXPECT_TRUE(is_text("héllo\n"));
}

TEST(Source, PathsAndLimits) {
  auto s = fresh("limits");
  std::string err;
  ASSERT_TRUE(s->create("p", "", 1, &err));
  for (const char* bad : {"/abs", "../up", "a/../b", "a//b", "", "a\\b", "./a"}) {
    EXPECT_FALSE(s->commit("p", 0, {{bad, "x"}}, "m", "me", 1, &err)) << bad;
  }
  EXPECT_FALSE(s->commit("p", 0, {{"big", std::string(kMaxFileBytes + 1, 'x')}}, "m", "me", 1, &err));
  EXPECT_FALSE(s->commit("p", 0, {{"a", "x"}, {"a", "y"}}, "m", "me", 1, &err));
  EXPECT_FALSE(s->commit("p", 0, {{"a", "x"}}, "", "me", 1, &err));  // a message is required
  ASSERT_TRUE(s->commit("p", 0, {{"dir/file", "x"}}, "m", "me", 1, &err)) << err;
  EXPECT_FALSE(s->commit("p", 1, {{"dir", "x"}}, "m", "me", 2, &err));  // a file where a directory is
  EXPECT_NE(err.find("directory"), std::string::npos) << err;
  EXPECT_TRUE(s->commit("p", 1, {{"dir", "x"}, {"dir/file", std::nullopt}}, "m", "me", 2, &err)) << err;
  // A host keeps a bounded number of projects.
  for (size_t i = 1; i < kMaxProjects; ++i) ASSERT_TRUE(s->create("q" + std::to_string(i), "", 1, &err)) << err;
  EXPECT_FALSE(s->create("one-too-many", "", 1, &err));
  EXPECT_NE(err.find("at most"), std::string::npos) << err;
}

TEST(Diff, MatchesDiffU) {
  std::string a = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n";
  std::string b = "one\ntwo\nTHREE\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\nthirteen\n";
  std::string d = unified_diff(a, b, "a/x", "b/x");
  EXPECT_EQ(d, "--- a/x\n+++ b/x\n@@ -1,6 +1,6 @@\n one\n two\n-three\n+THREE\n four\n five\n six\n"
               "@@ -10,3 +10,4 @@\n ten\n eleven\n twelve\n+thirteen\n");
  // Agrees with diff -u on the hunks, where diff is installed.
  auto dir = fs::path(GOOBER_TEST_TMP) / "source-diff";
  fs::create_directories(dir);
  base::write_file_durable(dir / "a", a);
  base::write_file_durable(dir / "b", b);
  auto r = base::run_process({"diff", "-u", (dir / "a").string(), (dir / "b").string()}, "", 1 << 20, 5000);
  if (r.started && r.exit_code == 1) {
    EXPECT_EQ(d.substr(d.find("@@")), r.out.substr(r.out.find("@@")));
  }
  EXPECT_EQ(unified_diff(a, a, "a", "b"), "");
  EXPECT_EQ(unified_diff("", "new\n", "/dev/null", "b/n"), "--- /dev/null\n+++ b/n\n@@ -0,0 +1,1 @@\n+new\n");
  EXPECT_EQ(unified_diff("old\n", "", "a/o", "/dev/null"), "--- a/o\n+++ /dev/null\n@@ -1,1 +0,0 @@\n-old\n");
  EXPECT_NE(unified_diff("x", "y", "a", "b").find("No newline"), std::string::npos);
  // Wildly different big files fall back to a whole-file replacement, quickly.
  std::string big_a, big_b;
  for (int i = 0; i < 20000; ++i) big_a += "a" + std::to_string(i) + "\n", big_b += "b" + std::to_string(i) + "\n";
  auto whole = unified_diff(big_a, big_b, "a", "b");
  EXPECT_NE(whole.find("@@ -1,20000 +1,20000 @@"), std::string::npos);
}

TEST(Source, DiffBetweenRevisions) {
  auto s = fresh("diffrev");
  std::string err;
  ASSERT_TRUE(s->create("p", "", 1, &err));
  ASSERT_TRUE(s->commit("p", 0, {{"a.txt", "one\n"}, {"bin", std::string("\0x", 2)}}, "1", "me", 1, &err));
  ASSERT_TRUE(s->commit("p", 1, {{"a.txt", "two\n"}, {"c.txt", "new\n"}, {"bin", std::string("\0y", 2)}}, "2", "me", 2, &err));
  auto d = s->diff("p", 1, 0, {}, &err);
  ASSERT_TRUE(d) << err;
  EXPECT_NE(d->find("-one\n+two\n"), std::string::npos) << *d;
  EXPECT_NE(d->find("--- /dev/null\n+++ b/c.txt"), std::string::npos) << *d;
  EXPECT_NE(d->find("Binary file bin changed"), std::string::npos) << *d;
  auto only = s->diff("p", 1, 2, {"c.txt"}, &err);
  EXPECT_EQ(only->find("a.txt"), std::string::npos);
}

}  // namespace
}  // namespace goober::source
