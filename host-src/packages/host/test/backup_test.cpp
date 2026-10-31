// Online backup and restore (PROGRESS.md Phase 3).

#include <gtest/gtest.h>

#include <unistd.h>

#include "goober/base/files.h"
#include "goober/base/time.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"

using namespace goober;
namespace fs = std::filesystem;

TEST(Backup, CopiesARunningHostAndRestoresIt) {
  auto root = fs::path(GOOBER_TEST_TMP) / "backup";
  fs::remove_all(root);
  auto config_for = [&](const std::string& storage) {
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": ")" + storage +
                                        R"(", "domains": {"localhost": {"tenant": "playground"}},
        "threads": {"network": 1, "boxes": 1}})",
                                    root, &err);
    EXPECT_TRUE(cfg) << err;
    return cfg;
  };
  std::string err;
  {
    host::Host h(config_for("storage"));
    h.start();
    ASSERT_TRUE(h.content().put("playground", "index.md", "# Home", &err)) << err;
    ASSERT_TRUE(h.identity().grant("playground", "me@example.com", "owner", "test", base::now_unix_ms(), &err)) << err;
    base::append_line(root / "storage/logs/access.log", "{}");
    // Backed up while the host is running and holding its databases open.
    host::BackupReport report;
    ASSERT_TRUE(host::backup_storage(root / "storage", root / "copy", &report, &err)) << err;
    EXPECT_GE(report.databases, 1);
    EXPECT_GE(report.files, 1);
    EXPECT_FALSE(host::backup_storage(root / "storage", root / "copy", &report, &err));  // never over a backup
  }
  EXPECT_TRUE(fs::exists(root / "copy/BACKUP.json"));
  EXPECT_FALSE(fs::exists(root / "copy/logs"));  // logs aren't kept

  host::BackupReport restored;
  EXPECT_FALSE(host::restore_storage(root / "copy", root / "storage", &restored, &err));  // not empty
  ASSERT_TRUE(host::restore_storage(root / "copy", root / "restored", &restored, &err)) << err;
  host::Host again(config_for("restored"));
  EXPECT_TRUE(again.content().get("playground", "index.md"));
  auto realm = again.identity().describe("playground", 2);
  EXPECT_EQ(realm.at("users").at(0).at("email"), "me@example.com");
}

// What a backup never holds can't stop it: an unreadable directory under tmp/ or a build cache
// is never entered. What it must hold and can't read fails with that path, owner, and mode,
// not the storage root (a site's nightly said only "cannot read .../storage").
TEST(Backup, UnreadableDirectoriesAreSkippedOrNamed) {
  if (::geteuid() == 0) GTEST_SKIP() << "root reads everything";
  auto root = fs::path(GOOBER_TEST_TMP) / "backup_unreadable";
  const char* kLocked[] = {"storage/tmp/source-build-1/locked", "storage/source/cache/p/locked", "storage/content/locked"};
  auto unlock = [&] {
    std::error_code ec;
    for (const char* d : kLocked) fs::permissions(root / d, fs::perms::owner_all, ec);
  };
  unlock();
  fs::remove_all(root);
  for (const char* d : kLocked) fs::create_directories(root / d);
  ASSERT_TRUE(base::write_file_durable(root / "storage/content/playground/index.md", "# Home"));
  ASSERT_TRUE(base::write_file_durable(root / "storage/tmp/source-build-1/locked/x", "x"));
  fs::permissions(root / "storage/tmp/source-build-1/locked", fs::perms::none);
  fs::permissions(root / "storage/source/cache/p/locked", fs::perms::none);

  host::BackupReport report;
  std::string err;
  ASSERT_TRUE(host::backup_storage(root / "storage", root / "copy1", &report, &err)) << err;
  EXPECT_TRUE(fs::exists(root / "copy1/content/playground/index.md"));

  fs::permissions(root / "storage/content/locked", fs::perms::none);
  EXPECT_FALSE(host::backup_storage(root / "storage", root / "copy2", &report, &err));
  EXPECT_NE(err.find("storage/content/locked (owner "), std::string::npos) << err;
  EXPECT_NE(err.find("mode 0000"), std::string::npos) << err;
  unlock();
}
