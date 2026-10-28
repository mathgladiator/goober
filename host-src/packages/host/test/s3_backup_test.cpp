// Backups to S3 and rehydrating a devbox (design/human/663.Backups.md;
// design/machine/019.Backups.md), against the stand-in AWS.

#include <gtest/gtest.h>

#include <chrono>

#include "../../aws/test/fake_aws.h"
#include "goober/base/files.h"
#include "goober/host/backup.h"
#include "goober/host/host.h"
#include "../src/keys.h"

using namespace goober;
namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace std::chrono;

namespace {

int64_t utc(int y, int mo, int d, int h, int mi) {
  return duration_cast<milliseconds>((sys_days(year(y) / mo / d) + hours(h) + minutes(mi)).time_since_epoch()).count();
}

const aws::Credentials kCreds{"AKIDTEST", "test-secret", ""};
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(2000, 'p');

class S3BackupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "s3-backup";
    fs::remove_all(root);
    fs::create_directories(root);
    base::write_file_durable(root / "host.json", R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1},
      "identity": {"mailer": "smtp", "smtp": {"url": "smtps://mail.example.com", "from": "a@example.com"}},
      "backup": {"s3": {"bucket": "backups", "endpoint": ")" + fake.endpoint() + R"(", "prefix": "host1"},
                 "nightly-at": "03:30", "keep-days": 14}})");
    std::string err;
    auto cfg = config::load_config(root / "host.json", &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(cfg, root / "host.json");
    host->set_backups_manual(true);
    host->start();
    ASSERT_TRUE(host->keys().set("system", "", "aws_access_key_id", kCreds.access_key_id, 1, &err)) << err;
    ASSERT_TRUE(host->keys().set("system", "", "aws_secret_access_key", kCreds.secret_access_key, 1, &err)) << err;
    ASSERT_TRUE(host->schemas().put(R"({"schema":"notes1","tables":{"notes":{"columns":{"text":{"type":"string"}}}}})", &err))
        << err;
    auto schema = host->schemas().get("notes1");
    ASSERT_TRUE(host->databases().open("playground", "notes", &*schema, &err)) << err;
    insert(100);
    ASSERT_TRUE(host->content().put("playground", "index.md", "# Home", &err)) << err;
    auto a = host->attachments().put("playground", kPng, "pic.png", "a picture", "public", 1, &err);
    ASSERT_TRUE(a) << err;
    attachment_id = a->id;
  }
  void TearDown() override { host.reset(); }

  void insert(int n) {
    std::string err;
    for (int i = 0; i < n; ++i) {
      ASSERT_TRUE(host->databases().operate("playground", "notes",
                                            "INSERT INTO notes (text) VALUES ('" + std::string(200, char('a' + i % 26)) + "')",
                                            nullptr, 10, true, &err))
          << err;
    }
  }
  std::vector<std::string> keys(const std::string& prefix) {
    std::vector<std::string> out;
    for (const auto& [k, v] : fake.objects("backups")) {
      if (k.starts_with("host1/" + prefix)) out.push_back(k.substr(6));
    }
    return out;
  }
  backup::Key key() {
    auto hex = host->keys().resolve("keys/system/backup_key", "");
    EXPECT_TRUE(hex);
    return *backup::parse_key(*hex);
  }
  host::RehydrateOptions rehydrate_options(const std::string& dir, int64_t at_ms) {
    host::RehydrateOptions o;
    o.location = {"backups", "us-east-1", fake.endpoint(), "host1/"};
    o.credentials = kCreds;
    o.key = key();
    o.dir = root / dir;
    o.at_ms = at_ms;
    o.port = 9123;
    return o;
  }
  int64_t notes_in(const fs::path& storage) {
    db::Databases dbs(storage);
    std::string err;
    auto r = dbs.operate("playground", "notes", "SELECT count(*) FROM notes", nullptr, 10, false, &err);
    return r ? r->at("rows").at(0).at(0).as_int64() : -1;
  }

  aws::testing::FakeAws fake{kCreds, 50};
  fs::path root;
  std::unique_ptr<host::Host> host;
  int64_t attachment_id = 0;
};

}  // namespace

TEST_F(S3BackupTest, NightlyIncrementsAssetsAndRehydrate) {
  int64_t t0 = utc(2026, 10, 27, 4, 0);
  // The first tick: the asset goes up, and (there being no night yet) the nightly runs.
  host->backup_tick(t0);
  auto nightly = keys("nightly/");
  ASSERT_EQ(nightly.size(), 1u);
  EXPECT_EQ(nightly[0], "nightly/2026-10-27T04-00-00Z.tar.gz.sealed");
  EXPECT_EQ(keys("assets/").size(), 1u);
  EXPECT_EQ(keys("assets/")[0], "assets/playground/" + std::to_string(attachment_id) + ".png.sealed");
  EXPECT_EQ(fake.refused_signatures(), 0);
  // Nothing in the bucket is readable: everything is sealed.
  for (const auto& [k, v] : fake.objects("backups")) {
    EXPECT_EQ(v.find("SQLite format 3"), std::string::npos) << k;
    EXPECT_TRUE(v.starts_with("GOOBERS1")) << k;
  }
  auto status = host->backup_status(false);
  EXPECT_EQ(status.at("night"), "2026-10-27T04-00-00Z");
  EXPECT_EQ(status.at("healthy"), true);
  EXPECT_TRUE(status.contains("key_warning"));  // nobody has confirmed keeping the key yet

  // Twenty minutes later, after more notes and a new page: an increment with only what changed.
  insert(5);
  std::string err;
  ASSERT_TRUE(host->content().put("playground", "about.md", "# About", &err)) << err;
  int64_t t1 = t0 + 20 * 60000;
  host->backup_tick(t1);
  auto increments = keys("increments/");
  ASSERT_GE(increments.size(), 2u);  // at least the notes delta and the manifest
  bool manifest = false, files = false;
  for (const auto& k : increments) {
    manifest |= k.ends_with("2026-10-27T04-20-00Z/MANIFEST.json.sealed");
    files |= k.ends_with("files.tar.gz.sealed");
  }
  EXPECT_TRUE(manifest);
  EXPECT_TRUE(files);
  // The notes delta holds a few pages, not the whole database.
  for (const auto& [k, v] : fake.objects("backups")) {
    if (k.ends_with(".pages.sealed")) EXPECT_LT(v.size(), 64u * 1024) << k;
  }
  // Nothing new: no new restore point.
  size_t before = fake.objects("backups").size();
  host->backup_tick(t1 + 16 * 60000);
  EXPECT_EQ(fake.objects("backups").size(), before);

  // Rehydrate the latest moment into a devbox.
  host::RehydrateReport report;
  ASSERT_TRUE(host::rehydrate(rehydrate_options("devbox", 0), &report, &err)) << err;
  EXPECT_EQ(report.night, "2026-10-27T04-00-00Z");
  EXPECT_EQ(report.increment, "2026-10-27T04-20-00Z");
  EXPECT_GE(report.deltas, 1);
  EXPECT_EQ(report.assets, 1);
  EXPECT_EQ(report.missing_assets, 0);
  EXPECT_TRUE(report.notes.empty()) << report.notes[0];
  EXPECT_EQ(notes_in(root / "devbox/storage"), 105);
  EXPECT_EQ(base::read_file(root / "devbox/storage/objects/playground/attachments" / std::to_string(attachment_id)), kPng);
  EXPECT_FALSE(fs::exists(root / "devbox/storage/keys"));  // the production keys stay behind
  // The devbox's config: a sandbox on localhost, the dev mailer, no backups.
  auto cfg = config::load_config(root / "devbox/host.json", &err);
  ASSERT_TRUE(cfg) << err;
  EXPECT_TRUE(cfg->sandbox);
  EXPECT_FALSE(cfg->backup.enabled());
  EXPECT_EQ(cfg->identity.mailer, "dev");
  EXPECT_EQ(cfg->listen_port, 9123);
  EXPECT_EQ(cfg->listen_address, "127.0.0.1");
  {
    host::Host devbox(cfg);
    EXPECT_TRUE(devbox.content().get("playground", "about.md"));  // a file from the increment
    EXPECT_TRUE(devbox.attachments().read("playground", attachment_id));
    EXPECT_FALSE(devbox.backup_run("nightly", t1, &err));  // a sandbox never backs up
  }

  // Ten minutes after the night: the night alone, before the five notes and the page.
  host::RehydrateReport early;
  ASSERT_TRUE(host::rehydrate(rehydrate_options("earlier", t0 + 10 * 60000), &early, &err)) << err;
  EXPECT_EQ(early.increment, "");
  EXPECT_EQ(notes_in(root / "earlier/storage"), 100);
  EXPECT_FALSE(fs::exists(root / "earlier/storage/content/playground/about.md"));

  // The wrong key reads nothing.
  auto wrong = rehydrate_options("wrong", 0);
  wrong.key = backup::new_key();
  EXPECT_FALSE(host::rehydrate(wrong, &report, &err));
  EXPECT_NE(err.find("wrong backup key"), std::string::npos) << err;
  EXPECT_FALSE(fs::exists(root / "wrong"));
  // A rehydrate that fails after unpacking the night leaves nothing behind: no keys, no data.
  for (const auto& [k, v] : fake.objects("backups")) {
    if (k.ends_with(".pages.sealed")) fake.remove_object("backups", k);
  }
  EXPECT_FALSE(host::rehydrate(rehydrate_options("broken", 0), &report, &err));
  EXPECT_FALSE(fs::exists(root / "broken")) << err;
  // Nor does anything rehydrate into a directory that isn't empty.
  EXPECT_FALSE(host::rehydrate(rehydrate_options("devbox", 0), &report, &err));
}

TEST_F(S3BackupTest, NightsRotateAfterKeepDays) {
  // Leftovers from three weeks ago, and from last week.
  for (const char* k : {"host1/nightly/2026-10-06T03-30-00Z.tar.gz.sealed",
                        "host1/increments/2026-10-06T03-30-00Z/2026-10-06T09-00-00Z/MANIFEST.json.sealed",
                        "host1/nightly/2026-10-20T03-30-00Z.tar.gz.sealed",
                        "host1/increments/2026-10-20T03-30-00Z/2026-10-20T09-00-00Z/MANIFEST.json.sealed"}) {
    fake.put_object("backups", k, "old");
  }
  int64_t t0 = utc(2026, 10, 27, 3, 31);
  host->backup_tick(t0);
  auto nightly = keys("nightly/");
  ASSERT_EQ(nightly.size(), 2u);
  EXPECT_EQ(nightly[0], "nightly/2026-10-20T03-30-00Z.tar.gz.sealed");
  EXPECT_EQ(nightly[1], "nightly/2026-10-27T03-31-00Z.tar.gz.sealed");
  auto increments = keys("increments/");
  ASSERT_EQ(increments.size(), 1u);
  EXPECT_TRUE(increments[0].starts_with("increments/2026-10-20T03-30-00Z/"));

  // The next night comes at 03:30 the next day, not before.
  host->backup_tick(utc(2026, 10, 28, 3, 0));
  EXPECT_EQ(keys("nightly/").size(), 2u);
  host->backup_tick(utc(2026, 10, 28, 3, 30));
  EXPECT_EQ(keys("nightly/").size(), 3u);

  // A deleted attachment stays in the bucket for keep-days, then goes.
  std::string err;
  ASSERT_TRUE(host->attachments().remove("playground", attachment_id, 2, &err)) << err;
  host->backup_run("assets", utc(2026, 10, 28, 4, 0), &err);
  EXPECT_EQ(keys("assets/").size(), 1u);
  host->backup_tick(utc(2026, 11, 12, 3, 30));  // a nightly 15 days on rotates it out
  EXPECT_EQ(keys("assets/").size(), 0u);
}

TEST_F(S3BackupTest, FailuresAreRecordedAndRetried) {
  int64_t t0 = utc(2026, 10, 27, 4, 0);
  fake.fail_next(100, 503);
  host->backup_tick(t0);
  EXPECT_TRUE(keys("nightly/").empty());
  auto status = host->backup_status(false);
  EXPECT_NE(std::string(status.at("last_error").as_string()).find("503"), std::string::npos) << json::dump(status);
  EXPECT_EQ(status.at("healthy"), false);
  fake.fail_next(0, 0);
  host->backup_tick(t0 + 60000);  // not yet: a failed nightly waits a while
  EXPECT_TRUE(keys("nightly/").empty());
  host->backup_tick(t0 + 16 * 60000);
  EXPECT_EQ(keys("nightly/").size(), 1u);
  EXPECT_EQ(host->backup_status(false).at("last_error"), "");
}
