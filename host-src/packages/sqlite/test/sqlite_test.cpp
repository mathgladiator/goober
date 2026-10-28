#include <gtest/gtest.h>

#include "goober/sqlite/sqlite.h"

namespace fs = std::filesystem;
using namespace goober::sqlite;

TEST(Sqlite, BindsStepsAndThrows) {
  fs::path dir = fs::path(GOOBER_TEST_TMP) / "sqlite";
  fs::remove_all(dir);
  fs::create_directories(dir);
  Db db(dir / "t.db");
  db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, n INTEGER)");
  Stmt(db, "INSERT INTO t (name, n) VALUES (?, ?)").bind(1, std::string_view("a")).bind(2, int64_t(7)).run();
  EXPECT_EQ(db.last_insert_rowid(), 1);
  Stmt q(db, "SELECT name, n FROM t WHERE id = ?");
  q.bind(1, int64_t(1));
  ASSERT_TRUE(q.step());
  EXPECT_EQ(q.text(0), "a");
  EXPECT_EQ(q.int64(1), 7);
  EXPECT_FALSE(q.step());
  EXPECT_THROW(db.exec("NOT SQL"), std::runtime_error);
}

TEST(Sqlite, BlobsHoldAnyBytes) {
  auto file = std::filesystem::path(GOOBER_TEST_TMP) / "sqlite-blob.db";
  std::filesystem::remove(file);
  goober::sqlite::Db db(file);
  db.exec("CREATE TABLE b (data BLOB)");
  std::string bytes("a\0b\xff\xfe", 5);
  goober::sqlite::Stmt(db, "INSERT INTO b VALUES (?)").bind_blob(1, bytes).run();
  goober::sqlite::Stmt s(db, "SELECT data, typeof(data) FROM b");
  ASSERT_TRUE(s.step());
  EXPECT_EQ(s.blob(0), bytes);
  EXPECT_EQ(s.text(1), "blob");
}
