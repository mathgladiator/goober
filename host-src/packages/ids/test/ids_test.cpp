// The id service (design/machine/038.Ids.md § 2): ranges vended durably, ids unique and
// increasing within a host, a counter that never goes back, and the jump past stored ids.

#include <gtest/gtest.h>

#include <set>
#include <thread>

#include "goober/ids/ids.h"
#include "goober/sqlite/sqlite.h"

using namespace goober;
namespace fs = std::filesystem;

namespace {
fs::path fresh(const std::string& name) {
  auto dir = fs::path(GOOBER_TEST_TMP) / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}
}  // namespace

TEST(Ids, StartAboveEveryRowidAndNeverRepeat) {
  auto dir = fresh("ids_basic");
  int64_t first = 0, last = 0;
  {
    ids::Service s(dir / "ids.db", "test");
    EXPECT_TRUE(s.created());
    first = s.next();
    EXPECT_EQ(first, ids::kFirstId);
    for (int i = 0; i < ids::kRangeLength + 5; ++i) last = s.next();  // crosses into a second range
    EXPECT_EQ(last, first + ids::kRangeLength + 5);
    EXPECT_EQ(s.ranges(10).size(), 2u);
    EXPECT_EQ(s.counter(), first + 2 * ids::kRangeLength);
  }
  // A restart loses the rest of its range: the next id is past everything vended.
  ids::Service again(dir / "ids.db", "test", /*floor=*/1);
  EXPECT_FALSE(again.created());
  EXPECT_EQ(again.next(), first + 2 * ids::kRangeLength);
}

TEST(Ids, ThreadsNeverShareAnId) {
  auto dir = fresh("ids_threads");
  ids::Service s(dir / "ids.db", "test");
  std::vector<std::vector<int64_t>> got(8);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 5000; ++i) got[t].push_back(s.next());
    });
  }
  for (auto& t : threads) t.join();
  std::set<int64_t> all;
  for (const auto& g : got) {
    for (size_t i = 1; i < g.size(); ++i) EXPECT_LT(g[i - 1], g[i]);  // increasing within a thread
    all.insert(g.begin(), g.end());
  }
  EXPECT_EQ(all.size(), 40000u);
}

TEST(Ids, AFloorAndARestoreMoveTheCounterPast) {
  auto dir = fresh("ids_floor");
  // Existing data above 2^32: a new counter starts past it.
  ids::Service s(dir / "ids.db", "test", ids::kFirstId + 123 + ids::kMargin);
  EXPECT_EQ(s.next(), ids::kFirstId + 123 + ids::kMargin);
  // A restore: everything from here is past the floor, and the range below it is dropped.
  s.move_past(ids::kFirstId + 50'000'000);
  EXPECT_EQ(s.next(), ids::kFirstId + 50'000'000);
  // The counter never goes back.
  s.move_past(5);
  EXPECT_GT(s.next(), ids::kFirstId + 50'000'000);
}

TEST(Ids, HighestIdReadsKeysAndAutoincrement) {
  auto dir = fresh("ids_highest");
  fs::create_directories(dir / "a" / "b");
  {
    sqlite::Db db(dir / "a" / "one.db");
    db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, email TEXT)");
    db.exec("INSERT INTO users (email) VALUES ('a'), ('b'), ('c')");
    db.exec("DELETE FROM users WHERE id = 3");  // AUTOINCREMENT still remembers 3
    db.exec("CREATE TABLE items (id INT PRIMARY KEY NOT NULL, name TEXT) STRICT");
    db.exec("INSERT INTO items VALUES (2, 'x')");
    db.exec("CREATE TABLE pairs (a INTEGER, b INTEGER, PRIMARY KEY (a, b))");  // no single key: skipped
    db.exec("INSERT INTO pairs VALUES (900, 900)");
    db.exec("CREATE TABLE \"odd \"\"name\" (id INTEGER PRIMARY KEY)");
    db.exec("INSERT INTO \"odd \"\"name\" VALUES (7)");
  }
  {
    sqlite::Db db(dir / "a" / "b" / "two.db");
    db.exec("CREATE TABLE t (k INTEGER PRIMARY KEY, v TEXT)");
    db.exec("INSERT INTO t VALUES (41, 'x')");
  }
  EXPECT_EQ(ids::highest_id(dir / "a" / "one.db"), 7);
  EXPECT_EQ(ids::highest_id_under(dir), 41);
  EXPECT_EQ(ids::highest_id(dir / "missing.db"), 0);
}
