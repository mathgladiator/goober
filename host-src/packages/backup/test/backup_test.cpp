#include <gtest/gtest.h>

#include <filesystem>
#include <cstring>
#include <fstream>

#include "goober/backup/backup.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"

namespace goober::backup {
namespace {

namespace fs = std::filesystem;

fs::path dir(const std::string& name) {
  auto d = fs::path(GOOBER_TEST_TMP) / ("backup-" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// A file shaped like a SQLite database: the header's magic and page size, then pages.
std::string fake_db(uint32_t page_size, int pages, char fill) {
  std::string data(size_t(page_size) * pages, fill);
  std::memcpy(data.data(), "SQLite format 3\0", 16);
  data[16] = char(page_size == 65536 ? 0 : page_size >> 8);
  data[17] = char(page_size == 65536 ? 1 : page_size & 0xff);
  for (int p = 0; p < pages; ++p) data[size_t(p) * page_size + 100] = char(p);
  return data;
}

TEST(Seal, RoundTripsAndRefusesTheWrongKeyOrATamperedFile) {
  auto d = dir("seal");
  Key key = new_key();
  std::string data(300000, 'x');
  for (size_t i = 0; i < data.size(); ++i) data[i] = char(i * 31);
  ASSERT_TRUE(base::write_file_durable(d / "plain", data));
  std::string err;
  ASSERT_TRUE(seal_file(key, d / "plain", d / "sealed", &err)) << err;
  auto sealed = base::read_file(d / "sealed");
  ASSERT_TRUE(sealed);
  EXPECT_EQ(sealed->size(), data.size() + 8 + 16 + 32);
  EXPECT_EQ(sealed->find(data.substr(1000, 64)), std::string::npos);  // nothing readable
  ASSERT_TRUE(unseal_file(key, d / "sealed", d / "back", &err)) << err;
  EXPECT_EQ(base::read_file(d / "back"), data);

  // Two seals of one file differ: a new salt, so new keys.
  ASSERT_TRUE(seal_file(key, d / "plain", d / "sealed2", &err)) << err;
  EXPECT_NE(base::read_file(d / "sealed2"), sealed);

  EXPECT_FALSE(unseal_file(new_key(), d / "sealed", d / "wrong", &err));
  EXPECT_NE(err.find("wrong backup key"), std::string::npos) << err;
  EXPECT_FALSE(fs::exists(d / "wrong"));
  std::string tampered = *sealed;
  tampered[5000] ^= 1;
  ASSERT_TRUE(base::write_file_durable(d / "tampered", tampered));
  EXPECT_FALSE(unseal_file(key, d / "tampered", d / "t", &err));
  EXPECT_FALSE(fs::exists(d / "t"));
  ASSERT_TRUE(base::write_file_durable(d / "short", sealed->substr(0, 40)));
  EXPECT_FALSE(unseal_file(key, d / "short", d / "s", &err));

  // An empty file seals too.
  ASSERT_TRUE(base::write_file_durable(d / "empty", ""));
  ASSERT_TRUE(seal_file(key, d / "empty", d / "empty.sealed", &err)) << err;
  ASSERT_TRUE(unseal_file(key, d / "empty.sealed", d / "empty.back", &err)) << err;
  EXPECT_EQ(base::read_file(d / "empty.back"), "");
}

TEST(Seal, KeysAreHex) {
  Key k = new_key();
  EXPECT_EQ(parse_key(key_hex(k)), k);
  EXPECT_EQ(parse_key(key_hex(k) + "\n"), k);
  EXPECT_FALSE(parse_key("abc"));
  EXPECT_FALSE(parse_key(std::string(64, 'g')));
}

TEST(Pages, DeltasHoldOnlyChangedPagesAndRebuildExactly) {
  auto d = dir("pages");
  std::string night = fake_db(4096, 20, 'a');
  ASSERT_TRUE(base::write_file_durable(d / "night.db", night));
  std::string err;
  auto base_map = map_pages(d / "night.db", &err);
  ASSERT_TRUE(base_map) << err;
  EXPECT_EQ(base_map->page_size, 4096u);
  EXPECT_EQ(base_map->pages, 20u);

  // Two pages change, and the file grows by one.
  std::string now = night;
  now[4096 * 3 + 7] = 'Z';
  now[4096 * 12 + 9] = 'Q';
  now += std::string(4096, 'n');
  ASSERT_TRUE(base::write_file_durable(d / "now.db", now));
  DeltaInfo info;
  ASSERT_TRUE(write_delta(&*base_map, d / "now.db", d / "delta", &info, &err)) << err;
  EXPECT_EQ(info.pages, 21u);
  EXPECT_EQ(info.changed, 3u);
  EXPECT_LT(fs::file_size(d / "delta"), 4u * 4096 + 200);
  fs::path base_path = d / "night.db";
  ASSERT_TRUE(apply_delta(&base_path, d / "delta", d / "rebuilt.db", &err)) << err;
  EXPECT_EQ(base::read_file(d / "rebuilt.db"), now);

  // Shrinking is kept too.
  std::string small = night.substr(0, 4096 * 5);
  ASSERT_TRUE(base::write_file_durable(d / "small.db", small));
  ASSERT_TRUE(write_delta(&*base_map, d / "small.db", d / "delta2", &info, &err)) << err;
  EXPECT_EQ(info.changed, 0u);
  ASSERT_TRUE(apply_delta(&base_path, d / "delta2", d / "rebuilt2.db", &err)) << err;
  EXPECT_EQ(base::read_file(d / "rebuilt2.db"), small);

  // A database new since the night has no base: every page goes.
  ASSERT_TRUE(write_delta(nullptr, d / "now.db", d / "delta3", &info, &err)) << err;
  EXPECT_EQ(info.changed, 21u);
  ASSERT_TRUE(apply_delta(nullptr, d / "delta3", d / "rebuilt3.db", &err)) << err;
  EXPECT_EQ(base::read_file(d / "rebuilt3.db"), now);

  // Against the wrong base, the hash catches it.
  ASSERT_TRUE(base::write_file_durable(d / "other.db", fake_db(4096, 20, 'b')));
  fs::path other = d / "other.db";
  EXPECT_FALSE(apply_delta(&other, d / "delta", d / "bad.db", &err));
  EXPECT_NE(err.find("wrong bytes"), std::string::npos) << err;
  EXPECT_FALSE(fs::exists(d / "bad.db"));

  // A changed page size makes every page new.
  ASSERT_TRUE(base::write_file_durable(d / "big.db", fake_db(8192, 10, 'a')));
  ASSERT_TRUE(write_delta(&*base_map, d / "big.db", d / "delta4", &info, &err)) << err;
  EXPECT_EQ(info.changed, 10u);

  // Not a database.
  ASSERT_TRUE(base::write_file_durable(d / "text", "hello"));
  EXPECT_FALSE(map_pages(d / "text", &err));
}

TEST(Names, StampsAndMoments) {
  int64_t t = 1793071800000;  // 2026-10-27T03:30:00Z
  EXPECT_EQ(stamp(t), "2026-10-27T03-30-00Z");
  EXPECT_EQ(parse_stamp("2026-10-27T03-30-00Z"), t);
  EXPECT_FALSE(parse_stamp("2026-13-27T03-30-00Z"));
  EXPECT_FALSE(parse_stamp("nope"));
  EXPECT_EQ(parse_moment("2026-10-27T03:30"), t);
  EXPECT_EQ(parse_moment("2026-10-27T03:30:00Z"), t);
  EXPECT_EQ(parse_moment("2026-10-27T03-30-00Z"), t);
  EXPECT_EQ(parse_moment("2026-10-27"), t + (20 * 3600 + 29 * 60 + 59) * 1000LL);  // the day's end
  EXPECT_FALSE(parse_moment("yesterday"));
}

TEST(Names, RestorePointsAreNightsAndTheirIncrements) {
  std::vector<std::string> keys = {
      "nightly/2026-10-26T03-30-00Z.tar.gz.sealed",
      "nightly/2026-10-27T03-30-00Z.tar.gz.sealed",
      "increments/2026-10-26T03-30-00Z/2026-10-26T12-00-00Z/MANIFEST.json.sealed",
      "increments/2026-10-26T03-30-00Z/2026-10-26T12-00-00Z/0.pages.sealed",
      "increments/2026-10-27T03-30-00Z/2026-10-27T09-15-00Z/MANIFEST.json.sealed",
      // An increment whose night is gone can't be restored.
      "increments/2026-10-01T03-30-00Z/2026-10-01T09-15-00Z/MANIFEST.json.sealed",
      "assets/playground/1.png.sealed",
  };
  auto points = restore_points(keys);
  ASSERT_EQ(points.size(), 4u);
  EXPECT_EQ(points.front().night, "2026-10-26T03-30-00Z");
  EXPECT_EQ(points.back().increment, "2026-10-27T09-15-00Z");
  auto latest = choose_point(points, 0);
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->increment, "2026-10-27T09-15-00Z");
  auto afternoon = choose_point(points, *parse_moment("2026-10-26T18:00"));
  ASSERT_TRUE(afternoon);
  EXPECT_EQ(afternoon->night, "2026-10-26T03-30-00Z");
  EXPECT_EQ(afternoon->increment, "2026-10-26T12-00-00Z");
  auto morning = choose_point(points, *parse_moment("2026-10-27T05:00"));
  EXPECT_EQ(morning->increment, "");  // the night itself
  EXPECT_FALSE(choose_point(points, *parse_moment("2026-10-20")));
}

}  // namespace
}  // namespace goober::backup

namespace goober::backup {
namespace {

// Review 5, finding 6: a sealed file is bound to its name, so one object can't stand in for
// another (an asset over another tenant's, an old night over a new one).
TEST(Seal, FilesAreBoundToTheirNames) {
  auto d = dir("seal-context");
  Key key = new_key();
  ASSERT_TRUE(base::write_file_durable(d / "plain", "tenant a's picture"));
  std::string err;
  ASSERT_TRUE(seal_file(key, d / "plain", d / "sealed", &err, "assets/a/1.png.sealed")) << err;
  EXPECT_TRUE(unseal_file(key, d / "sealed", d / "back", &err, "assets/a/1.png.sealed")) << err;
  EXPECT_FALSE(unseal_file(key, d / "sealed", d / "moved", &err, "assets/b/7.png.sealed"));
  EXPECT_FALSE(fs::exists(d / "moved"));
  EXPECT_FALSE(unseal_file(key, d / "sealed", d / "bare", &err));
}

}  // namespace
}  // namespace goober::backup
