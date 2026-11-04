// Online backup and restore (PROGRESS.md Phase 3).

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>

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

// Local nightly copies (046 A12): a backup into a dated directory keeps its dated siblings to the
// newest 3 (S3 backups on) or 14 (off). Only real directories named YYYY-MM-DD are touched.
TEST(Backup, LocalCopiesKeepTheNewestDatedOnes) {
  EXPECT_EQ(host::local_copies_to_keep(true), 3);
  EXPECT_EQ(host::local_copies_to_keep(false), 14);
  EXPECT_TRUE(host::is_dated_copy_name("2026-11-03"));
  for (const char* bad : {"2026-13-01", "2026-11-00", "2026-11-3", "x026-11-03", "2026_11_03", "2026-11-03.old"}) {
    EXPECT_FALSE(host::is_dated_copy_name(bad)) << bad;
  }
  auto root = fs::path(GOOBER_TEST_TMP) / "backup_local";
  auto setup = [&] {
    fs::remove_all(root);
    fs::create_directories(root / "storage/content");
    base::write_file_durable(root / "storage/content/a.md", "# A");
    for (int day = 1; day <= 20; ++day) {
      char name[16];
      std::snprintf(name, sizeof name, "2026-10-%02d", day);
      fs::create_directories(root / "backups" / name);
      base::write_file_durable(root / "backups" / name / "BACKUP.json", R"({"bytes": 0})");  // a copy a backup made
    }
    // Dated but not a backup's (no BACKUP.json): someone else's, never removed (security L3).
    fs::create_directories(root / "backups/2026-08-01/photos");
    // None of these is a dated copy: they stay, whatever their age.
    base::write_file_durable(root / "backups/2026-09-01", "a file");
    fs::create_directories(root / "backups/notes");
    fs::create_directories(root / "backups/2026-13-01");
    fs::create_directories(root / "elsewhere/2026-09-02");
    fs::create_directory_symlink(root / "elsewhere/2026-09-02", root / "backups/2026-09-02");
  };
  auto dated = [&] {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(root / "backups")) {
      std::string n = e.path().filename().string();
      if (host::is_dated_copy_name(n) && !e.is_symlink() && e.is_directory()) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    return names;
  };
  std::string err;
  host::BackupReport report;

  setup();
  ASSERT_TRUE(host::backup_storage(root / "storage", root / "backups/2026-10-21", &report, &err)) << err;
  auto removed = host::prune_local_copies(root / "backups/2026-10-21", 3, &err);
  EXPECT_EQ(removed.size(), 18u);
  EXPECT_EQ(removed.front(), "2026-10-01");
  EXPECT_EQ(dated(), (std::vector<std::string>{"2026-08-01", "2026-10-19", "2026-10-20", "2026-10-21"}));
  for (const char* kept : {"2026-09-01", "notes", "2026-13-01", "2026-09-02", "2026-08-01/photos"}) {
    EXPECT_TRUE(fs::exists(fs::symlink_status(root / "backups" / kept))) << kept;
  }
  EXPECT_TRUE(fs::exists(root / "elsewhere/2026-09-02"));  // a link's target is never followed

  // S3 off: fourteen.
  setup();
  ASSERT_TRUE(host::backup_storage(root / "storage", root / "backups/2026-10-21", &report, &err)) << err;
  host::prune_local_copies(root / "backups/2026-10-21", 14, &err);
  auto fourteen = dated();
  ASSERT_EQ(fourteen.size(), 15u);
  EXPECT_EQ(fourteen.at(0), "2026-08-01");
  EXPECT_EQ(fourteen.at(1), "2026-10-08");

  // A trailing slash names the same directory (code review C8).
  setup();
  ASSERT_TRUE(host::backup_storage(root / "storage", root / "backups/2026-10-21", &report, &err)) << err;
  EXPECT_EQ(host::dated_copy_dir((root / "backups/2026-10-21").string() + "/"), (root / "backups/2026-10-21").lexically_normal());
  EXPECT_TRUE(host::dated_copy_dir(root / "backups/manual/").empty());
  EXPECT_EQ(host::prune_local_copies((root / "backups/2026-10-21").string() + "/", 3, &err).size(), 18u);
  EXPECT_EQ(dated(), (std::vector<std::string>{"2026-08-01", "2026-10-19", "2026-10-20", "2026-10-21"}));

  // This night's copy stays even when its name is older than the rest.
  setup();
  ASSERT_TRUE(host::backup_storage(root / "storage", root / "backups/2026-01-01", &report, &err)) << err;
  host::prune_local_copies(root / "backups/2026-01-01", 3, &err);
  EXPECT_EQ(dated(), (std::vector<std::string>{"2026-01-01", "2026-08-01", "2026-10-19", "2026-10-20"}));

  // A directory not named for a date prunes nothing.
  setup();
  ASSERT_TRUE(host::backup_storage(root / "storage", root / "backups/manual", &report, &err)) << err;
  EXPECT_TRUE(host::prune_local_copies(root / "backups/manual", 3, &err).empty());
  EXPECT_EQ(dated().size(), 21u);

  // Counted: a whole copy by its BACKUP.json, one a failed run left by measuring it.
  base::write_file_durable(root / "backups/2026-08-01/partial.db", std::string(1000, 'x'));
  auto copies = host::local_copies(root / "backups");
  EXPECT_EQ(copies.copies, 21);
  EXPECT_EQ(copies.bytes, 1000);
  EXPECT_EQ(host::local_copies_dir(root / "storage"), fs::absolute(root / "backups").lexically_normal());
}

// The limits check names the local copies' bytes, and how many backup keeps by the config.
TEST(Backup, LimitsReportTheLocalCopies) {
  auto root = fs::path(GOOBER_TEST_TMP) / "backup_local_limits";
  fs::remove_all(root);
  auto config_for = [&](const std::string& backup) {
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1})" + backup + "}",
                                    root, &err);
    EXPECT_TRUE(cfg) << err;
    return cfg;
  };
  std::string err;
  {
    host::Host h(config_for(""));
    ASSERT_TRUE(h.content().put("playground", "index.md", "# Home", &err)) << err;
    host::BackupReport report;
    ASSERT_TRUE(host::backup_storage(root / "storage", root / "backups/2026-11-01", &report, &err)) << err;
    auto row = h.limits_report(true).at("local-copies").as_object();
    EXPECT_EQ(row.at("copies").as_int64(), 1);
    EXPECT_EQ(row.at("used_bytes").as_int64(), report.bytes);
    EXPECT_EQ(row.at("keep").as_int64(), 14);
    EXPECT_TRUE(row.contains("dir"));
    // /goober/ready is public: the copies, their count, bytes, and path, aren't there at all.
    EXPECT_FALSE(h.limits_report(false).contains("local-copies"));
  }
  host::Host s3(config_for(R"(, "backup": {"s3": {"bucket": "goober-backups", "region": "us-east-1"}})"));
  EXPECT_EQ(s3.limits_report(true).at("local-copies").at("keep").as_int64(), 3);
}
