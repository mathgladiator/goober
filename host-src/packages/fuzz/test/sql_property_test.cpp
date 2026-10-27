// A property of the SQL authorizer (008 §5; 013.SecurityReview.md): whatever statements a
// box sends through a strategy granted only the "open" table, the "secret" table is never
// changed and its values never come back, directly or by way of "open".

#include <gtest/gtest.h>

#include <random>

#include "fuzz.h"
#include "goober/db/database.h"
#include "goober/json/json.h"
#include "goober/wal/wal.h"

using namespace goober;
namespace fs = std::filesystem;

namespace {

const std::vector<std::string> kTables = {"open", "secret", "main.secret", "\"secret\"", "SECRET", "temp.secret",
                                          "[secret]", "`secret`", "sqlite_master", "goober_schemas"};
const std::vector<std::string> kWheres = {"", " WHERE 1", " WHERE id IN (SELECT id FROM {t2})", " WHERE v LIKE '%'",
                                          " WHERE EXISTS (SELECT 1 FROM {t2})"};
const std::vector<std::string> kShapes = {
    "SELECT * FROM {t}{w}",
    "SELECT v FROM {t}{w}",
    "SELECT (SELECT group_concat(v) FROM {t})",
    "SELECT json_group_array(v) FROM {t}",
    "SELECT * FROM {t} JOIN {t2} ON 1",
    "SELECT * FROM (SELECT * FROM {t})",
    "WITH c AS (SELECT v FROM {t}) SELECT * FROM c",
    "WITH c AS (SELECT v FROM {t}) INSERT INTO {t2} (v) SELECT v FROM c",
    "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 3) DELETE FROM {t} WHERE id IN r",
    "INSERT INTO {t} (v) VALUES ('x')",
    "INSERT INTO {t} (v) SELECT v FROM {t2}",
    "INSERT INTO {t} SELECT * FROM {t2}",
    "INSERT INTO {t} (v) VALUES ((SELECT v FROM {t2} LIMIT 1))",
    "INSERT INTO {t} (id, v) VALUES (1, 'u') ON CONFLICT (id) DO UPDATE SET v = excluded.v",
    "INSERT INTO {t} (id, v) VALUES (1, 'u') ON CONFLICT (id) DO UPDATE SET v = (SELECT v FROM {t2} LIMIT 1)",
    "REPLACE INTO {t} (id, v) VALUES (1, 'r')",
    "INSERT OR REPLACE INTO {t} (id, v) VALUES (1, 'r')",
    "UPDATE OR REPLACE {t} SET id = 1",
    "UPDATE {t} SET v = 'p'{w}",
    "UPDATE {t} SET v = (SELECT v FROM {t2} LIMIT 1){w}",
    "UPDATE open SET v = 'q' FROM {t2} WHERE open.id = {t2}.id",
    "DELETE FROM {t}{w}",
    "DELETE FROM open RETURNING (SELECT v FROM {t2} LIMIT 1)",
    "INSERT INTO open (v) VALUES ('y') RETURNING (SELECT group_concat(v) FROM {t2})",
    "CREATE TRIGGER t AFTER INSERT ON open BEGIN DELETE FROM {t2}; END",
    "CREATE VIEW leak AS SELECT * FROM {t}",
    "CREATE TEMP TABLE x AS SELECT * FROM {t}",
    "DROP TABLE {t}",
    "ALTER TABLE {t} RENAME TO gone",
    "ATTACH 'x.db' AS y",
    "PRAGMA writable_schema = 1",
    "SELECT * FROM pragma_table_info('secret')",
    "SELECT load_extension('x')",
    "SELECT * FROM json_each((SELECT json_group_array(v) FROM {t}))",
    "VACUUM",
    "BEGIN",
    "SAVEPOINT a",
    "SELECT 1; DELETE FROM {t}",
};

std::string fill(std::mt19937_64& rng, std::string s) {
  for (std::string key : {"{w}", "{t2}", "{t}"}) {
    for (size_t at = s.find(key); at != std::string::npos; at = s.find(key)) {
      std::string v = key == "{w}" ? kWheres[rng() % kWheres.size()] : kTables[rng() % kTables.size()];
      s.replace(at, key.size(), v);
    }
  }
  return s;
}

}  // namespace

TEST(SqlProperty, NoGrantNoTouch) {
  auto root = fs::path(GOOBER_TEST_TMP) / "sql_property";
  fs::remove_all(root);
  wal::HostWal w(root);
  db::SchemaStore schemas(w);
  db::Databases dbs(root);
  std::string err;
  ASSERT_TRUE(schemas.put(R"({"schema":"prop1","tables":{"open":{"columns":{"v":{"type":"string"}}},
                                                       "secret":{"columns":{"v":{"type":"string"}}}}})",
                          &err))
      << err;
  manifest::DataStrategy st;
  st.name = "s";
  st.schema = "prop1";
  st.database = "prop";
  st.pattern = "sql";
  manifest::TableGrant all;
  all.read_all = all.insert = all.update_all = all.remove = true;
  st.tables["open"] = all;  // and nothing on "secret"
  st.row_limit = 50;
  st.step_budget = 100000;
  std::vector<manifest::DataStrategy> strategies{st};
  auto schema = schemas.get("prop1");
  ASSERT_TRUE(dbs.open("t", "prop", &*schema, &err)) << err;
  ASSERT_TRUE(dbs.operate("t", "prop", "INSERT INTO secret (v) VALUES ('TOPSECRET'), ('TOPSECRET'), ('TOPSECRET')",
                          nullptr, 10, true, &err))
      << err;
  ASSERT_TRUE(dbs.operate("t", "prop", "INSERT INTO open (v) VALUES ('a'), ('b')", nullptr, 10, true, &err)) << err;

  std::mt19937_64 rng(20261026);
  int allowed = 0, statements = fuzz::iterations(3000);
  for (int batch = 0; batch < statements / 50; ++batch) {
    db::Session session(dbs, "t", strategies, schemas);
    for (int i = 0; i < 50; ++i) {
      std::string sql = fill(rng, kShapes[rng() % kShapes.size()]);
      // Now and then, a mutated statement too, for the parser's corners.
      if (rng() % 4 == 0) sql = fuzz::mutate(rng, sql, {" ", "secret", "open", "(", ")", ",", "'", ";", "--", "/*", "*/"});
      std::string result = session.run("s", sql, "");
      if (result.find("was rolled back") != std::string::npos && std::getenv("GOOBER_FUZZ_VERBOSE")) {
        fprintf(stderr, "rolled back by: %s\n", sql.c_str());
      }
      if (result.find("TOPSECRET") != std::string::npos) {
        ADD_FAILURE() << "secret values came back from: " << sql << "\n" << result;
        return;
      }
      allowed += result.find("\"error\"") == std::string::npos;
      // Did that statement carry secret values into "open"? Asked through the box's own view.
      std::string probe = session.run("s", "SELECT count(*) FROM open WHERE v = 'TOPSECRET'", "");
      if (probe.find("[[0]]") == std::string::npos && probe.find("\"error\"") == std::string::npos) {
        ADD_FAILURE() << "secret values reached open by: " << sql << "\n" << result;
        return;
      }
    }
    if (rng() % 2) {
      // A statement SQLite rolled back by itself spoils the session: its commit must fail.
      if (!session.commit(&err)) ASSERT_NE(err.find("rolled back"), std::string::npos) << err;
    } else {
      session.rollback();
    }
  }
  // The secret table is exactly as it was, and none of it leaked into "open".
  auto secret = dbs.operate("t", "prop", "SELECT count(*), sum(v = 'TOPSECRET') FROM secret", nullptr, 10, false, &err);
  ASSERT_TRUE(secret) << err;
  EXPECT_EQ(json::dump(secret->at("rows")), "[[3,3]]");
  auto open = dbs.operate("t", "prop", "SELECT count(*) FROM open WHERE v = 'TOPSECRET'", nullptr, 10, false, &err);
  EXPECT_EQ(json::dump(open->at("rows")), "[[0]]");
  auto tables = dbs.operate("t", "prop", "SELECT count(*) FROM sqlite_master WHERE type IN ('trigger', 'view')", nullptr, 10,
                            false, &err);
  EXPECT_EQ(json::dump(tables->at("rows")), "[[0]]");
  EXPECT_GT(allowed, statements / 100);  // the property is about real statements, not only refusals
}
