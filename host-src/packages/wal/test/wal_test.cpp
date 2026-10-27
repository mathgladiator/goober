#include <gtest/gtest.h>

#include <fstream>

#include "goober/base/files.h"
#include "goober/wal/wal.h"

using namespace goober;
namespace fs = std::filesystem;

namespace {
fs::path fresh(const char* name) {
  fs::path p = fs::path(GOOBER_TEST_TMP) / name;
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}
}  // namespace

TEST(Wal, CommitAppliesWritesAndDeletes) {
  auto root = fresh("wal_commit");
  wal::HostWal w(root);
  std::string err;
  ASSERT_TRUE(w.commit({{wal::FileChange::Kind::kWrite, "a/b.txt", "one"},
                        {wal::FileChange::Kind::kWrite, "c.txt", "two"}},
                       &err))
      << err;
  EXPECT_EQ(*base::read_file(root / "a/b.txt"), "one");
  ASSERT_TRUE(w.commit({{wal::FileChange::Kind::kDelete, "c.txt", ""}}, &err));
  EXPECT_FALSE(base::read_file(root / "c.txt").has_value());
}

TEST(Wal, RefusesUnsafePaths) {
  auto root = fresh("wal_unsafe");
  wal::HostWal w(root);
  std::string err;
  EXPECT_FALSE(w.commit({{wal::FileChange::Kind::kWrite, "../escape.txt", "x"}}, &err));
  EXPECT_FALSE(fs::exists(root.parent_path() / "escape.txt"));
}

TEST(Wal, ReplaysCommittedBundleAfterCrash) {
  auto root = fresh("wal_replay");
  {
    wal::HostWal w(root);
    ASSERT_TRUE(w.commit_without_apply_for_test({{wal::FileChange::Kind::kWrite, "late.txt", "arrived"}}));
    EXPECT_FALSE(fs::exists(root / "late.txt"));
  }
  wal::HostWal again(root);
  EXPECT_EQ(again.recovery().replayed_bundles, 1u);
  EXPECT_EQ(*base::read_file(root / "late.txt"), "arrived");
}

TEST(Wal, DiscardsTornTail) {
  auto root = fresh("wal_torn");
  {
    wal::HostWal w(root);
    ASSERT_TRUE(w.commit_without_apply_for_test({{wal::FileChange::Kind::kWrite, "ok.txt", "yes"}}));
  }
  {
    std::ofstream out(root / "wal/host.wal", std::ios::app | std::ios::binary);
    out << "\x40\x00\x00\x00garbage";
  }
  wal::HostWal again(root);
  EXPECT_TRUE(again.recovery().torn_tail);
  EXPECT_EQ(again.recovery().replayed_bundles, 1u);
  EXPECT_EQ(*base::read_file(root / "ok.txt"), "yes");
}

TEST(Wal, UncommittedBundleIsDiscarded) {
  auto root = fresh("wal_uncommitted");
  {
    wal::HostWal w(root);
    ASSERT_TRUE(w.commit_without_apply_for_test({{wal::FileChange::Kind::kWrite, "x.txt", "x"}}));
  }
  // Chop off the commit record (the last 25 bytes: 8 frame + 17 payload).
  auto file = root / "wal/host.wal";
  auto size = fs::file_size(file);
  fs::resize_file(file, size - 25);
  wal::HostWal again(root);
  EXPECT_EQ(again.recovery().replayed_bundles, 0u);
  EXPECT_EQ(again.recovery().discarded_bundles, 1u);
  EXPECT_FALSE(fs::exists(root / "x.txt"));
}

TEST(Wal, TruncatesWhenLarge) {
  auto root = fresh("wal_truncate");
  wal::HostWal w(root, 1024);
  std::string err;
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(w.commit({{wal::FileChange::Kind::kWrite, "f.txt", std::string(200, 'x')}}, &err));
  }
  EXPECT_LT(fs::file_size(root / "wal/host.wal"), 2048u);
}

TEST(Wal, ABundleThatCantApplyIsSetAsideAndNeverLost) {
  auto root = fresh("wal_failed");
  {
    wal::HostWal w(root, 1);  // truncate after every commit, if it may
    std::string err;
    ASSERT_TRUE(w.commit({{wal::FileChange::Kind::kWrite, "a", "file"}}, &err)) << err;
    // "a" is a file, so "a/b" can't be written: committed, but not applied.
    EXPECT_FALSE(w.commit({{wal::FileChange::Kind::kWrite, "a/b", "x"}}, &err));
    ASSERT_TRUE(w.commit({{wal::FileChange::Kind::kWrite, "c", "y"}}, &err)) << err;
    EXPECT_FALSE(w.commit({{wal::FileChange::Kind::kWrite, std::string(300, 'n'), "too long a name"}}, &err));
  }
  wal::HostWal again(root);  // starts anyway
  ASSERT_EQ(again.recovery().failed.size(), 1u);
  EXPECT_NE(again.recovery().failed[0].find("cannot apply"), std::string::npos);
  EXPECT_TRUE(fs::exists(root / "wal" / "failed"));
  EXPECT_EQ(*base::read_file(root / "c"), "y");
}
