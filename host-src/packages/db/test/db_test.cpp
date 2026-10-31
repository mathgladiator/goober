#include <gtest/gtest.h>

#include "goober/db/database.h"

using namespace goober;
namespace fs = std::filesystem;

namespace {

const char* kGuestbook1 = R"({"schema": "guestbook1", "tables": {"entries": {"columns": {
    "name": {"type": "string", "max-length": 20},
    "message": {"type": "string"},
    "created_ms": {"type": "int64"},
    "approved": {"type": "boolean", "default": false}},
    "indexes": {"entries_by_created": {"columns": ["created_ms"]}}}}})";

manifest::DataStrategy strategy(manifest::TableGrant grant, std::string schema = "guestbook1") {
  manifest::DataStrategy s;
  s.name = "book";
  s.schema = std::move(schema);
  s.database = "guestbook";
  s.pattern = "sql";
  s.tables["entries"] = std::move(grant);
  s.row_limit = 10;
  return s;
}

boost::json::value J(const std::string& text) {
  std::string err;
  auto v = json::parse(text, &err);
  EXPECT_TRUE(v) << text;
  return v.value_or(nullptr);
}

}  // namespace

class DbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "db";
    fs::remove_all(root);
    w = std::make_unique<wal::HostWal>(root);
    schemas = std::make_unique<db::SchemaStore>(*w);
    dbs = std::make_unique<db::Databases>(root);
    std::string err;
    ASSERT_TRUE(schemas->put(kGuestbook1, &err)) << err;
  }
  fs::path root;
  std::unique_ptr<wal::HostWal> w;
  std::unique_ptr<db::SchemaStore> schemas;
  std::unique_ptr<db::Databases> dbs;
};

TEST(Schema, ParsesAndRefuses) {
  std::string err;
  auto s = db::parse_schema(kGuestbook1, &err);
  ASSERT_TRUE(s) << err;
  EXPECT_EQ(s->tables[0].columns[0].name, "id");  // added
  EXPECT_TRUE(s->tables[0].columns[0].primary_key);
  EXPECT_EQ(s->tables[0].column("approved")->sql(), "\"approved\" INTEGER NOT NULL DEFAULT 0 CHECK (\"approved\" IN (0, 1))");
  auto refuse = [&](const char* text, const char* why) {
    std::string e;
    EXPECT_FALSE(db::parse_schema(text, &e)) << text;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"schema":"s1","tables":{"t":{"columns":{"x":{"type":"int"}}}}})", "type must be");
  refuse(R"({"schema":"s1","tables":{"goober_t":{"columns":{"x":{"type":"int64"}}}}})", "table name");
  refuse(R"({"schema":"s1","tables":{"t":{"columns":{"x":{"type":"int64","default":"no"}}}}})", "default");
  refuse(R"({"schema":"s1","tables":{"t":{"columns":{"x":{"type":"int64"}},"indexes":{"i":{"columns":["y"]}}}}})", "every column");
}

TEST_F(DbTest, SchemasAreStoredOnceByName) {
  std::string err;
  EXPECT_TRUE(schemas->put(kGuestbook1, &err)) << err;  // the same content again is fine
  EXPECT_FALSE(schemas->put(R"({"schema":"guestbook1","tables":{"x":{"columns":{"a":{"type":"string"}}}}})", &err));
  EXPECT_NE(err.find("already stored"), std::string::npos);
  EXPECT_EQ(schemas->list(), (std::vector<std::string>{"guestbook1"}));
}

TEST_F(DbTest, SessionsRunGrantedSqlInOneTransaction) {
  manifest::TableGrant g;
  g.read_all = true;
  g.insert = true;
  g.update.insert("approved");
  std::vector<manifest::DataStrategy> strategies{strategy(g)};
  {
    db::Session s(*dbs, "playground", strategies, *schemas);
    auto r = J(s.run("book", "INSERT INTO entries (name, message, created_ms) VALUES (?, ?, ?)", R"(["Ada", "Hi", 5])"));
    ASSERT_FALSE(r.as_object().contains("error")) << json::dump(r);
    EXPECT_EQ(r.at("changes"), 1);
    EXPECT_EQ(r.at("last_insert_id"), 1);
    auto q = J(s.run("book", "SELECT name, approved FROM entries WHERE created_ms = :at", R"({"at": 5})"));
    EXPECT_EQ(json::dump(q.at("rows")), R"([["Ada",0]])");
    std::string err;
    ASSERT_TRUE(s.commit(&err)) << err;
  }
  {
    // Rolled back: destroyed without a commit.
    db::Session s(*dbs, "playground", strategies, *schemas);
    s.run("book", "INSERT INTO entries (name, message, created_ms) VALUES ('Bob', 'x', 6)", "");
  }
  db::Session s(*dbs, "playground", strategies, *schemas);
  auto all = J(s.run("book", "SELECT json_group_array(name) FROM entries", ""));
  EXPECT_EQ(json::dump(all.at("rows")), R"([["[\"Ada\"]"]])");

  auto denied = [&](const char* sql, const char* why) {
    auto r = J(s.run("book", sql, ""));
    ASSERT_TRUE(r.as_object().contains("error")) << sql;
    EXPECT_NE(std::string(r.at("error").as_string()).find(why), std::string::npos) << json::dump(r);
  };
  denied("UPDATE entries SET name = 'x'", "can't update entries.name");
  denied("DELETE FROM entries", "can't delete");
  denied("SELECT * FROM goober_schemas", "can't read table goober_schemas");
  denied("SELECT random()", "pure functions");
  denied("WITH x AS (SELECT name FROM goober_schemas) SELECT * FROM x", "goober_schemas");
  denied("SELECT * FROM (SELECT * FROM sqlite_master)", "sqlite_master");
  denied("PRAGMA table_info(entries)", "PRAGMA");
  denied("ATTACH 'x.db' AS x", "ATTACH");
  denied("DROP TABLE entries", "schema changes");
  denied("BEGIN", "transactions");
  denied("SELECT 1; SELECT 2", "one statement");
  denied("INSERT INTO entries (name, message, created_ms) VALUES ('this name is far too long', 'x', 1)", "CHECK");
  auto r = J(s.run("book", "UPDATE entries SET approved = 1", ""));
  EXPECT_EQ(r.at("changes"), 1);
  // The tenant's database is its own file.
  EXPECT_TRUE(dbs->exists("playground", "guestbook"));
  EXPECT_FALSE(dbs->exists("second", "guestbook"));
}

TEST_F(DbTest, LimitsAndBudgets) {
  manifest::TableGrant g;
  g.read_all = true;
  g.insert = true;
  auto st = strategy(g);
  st.step_budget = 5000;
  std::vector<manifest::DataStrategy> strategies{st};
  db::Session s(*dbs, "playground", strategies, *schemas);
  auto many = J(s.run("book", "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20) SELECT i FROM n", ""));
  EXPECT_NE(std::string(many.at("error").as_string()).find("more than 10 rows"), std::string::npos);
  auto slow = J(s.run("book", "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 1000000) SELECT count(*) FROM n", ""));
  EXPECT_NE(std::string(slow.at("error").as_string()).find("step budget"), std::string::npos) << json::dump(slow);
  // A value that doubles itself 29 times would be a gigabyte; it stops at the length limit.
  auto huge = J(s.run("book", "WITH RECURSIVE c(s, n) AS (SELECT 'x', 0 UNION ALL SELECT s || s, n + 1 FROM c WHERE n < 29) "
                              "SELECT length(s) FROM c ORDER BY n DESC LIMIT 1", ""));
  ASSERT_TRUE(huge.as_object().contains("error")) << json::dump(huge);
  EXPECT_NE(std::string(huge.at("error").as_string()).find("too big"), std::string::npos) << json::dump(huge);
  // REPLACE would delete the row it collides with, behind the grants.
  for (const char* sql : {"REPLACE INTO entries (id, name, message, created_ms) VALUES (1, 'x', 'y', 1)",
                          "INSERT OR REPLACE INTO entries (id, name, message, created_ms) VALUES (1, 'x', 'y', 1)",
                          "INSERT /* hi */ OR\nreplace INTO entries (id, name, message, created_ms) VALUES (1, 'x', 'y', 1)"}) {
    auto r = J(s.run("book", sql, ""));
    ASSERT_TRUE(r.as_object().contains("error")) << sql;
    EXPECT_NE(std::string(r.at("error").as_string()).find("REPLACE"), std::string::npos) << sql;
  }
  // SQL is a view into box memory with nothing after it: the host never reads past its end.
  std::string buffer = "SELECT 1; DROP TABLE entries";
  auto edge = J(s.run("book", std::string_view(buffer.data(), 8), ""));
  EXPECT_FALSE(edge.as_object().contains("error")) << json::dump(edge);
  // OR ROLLBACK would end the request's transaction.
  auto rb = J(s.run("book", "INSERT OR ROLLBACK INTO entries (name, message, created_ms) VALUES ('a', 'b', 1)", ""));
  EXPECT_NE(std::string(rb.at("error").as_string()).find("ROLLBACK"), std::string::npos) << json::dump(rb);
  // replace() the function, and the word inside a string, are fine.
  auto fn = J(s.run("book", "SELECT replace('a-b', '-', ' '), 'or replace into' AS t", ""));
  EXPECT_FALSE(fn.as_object().contains("error")) << json::dump(fn);
}

TEST_F(DbTest, ARolledBackTransactionSpoilsTheRequest) {
  manifest::TableGrant g;
  g.read_all = g.insert = g.update_all = g.remove = true;
  auto st = strategy(g);
  st.step_budget = 5000;
  std::vector<manifest::DataStrategy> strategies{st};
  {
    db::Session s(*dbs, "playground", strategies, *schemas);
    ASSERT_FALSE(J(s.run("book", "INSERT INTO entries (name, message, created_ms) VALUES ('a', 'b', 1)", "")).as_object().contains("error"));
    // A runaway write is interrupted, and SQLite rolls the whole transaction back by itself.
    J(s.run("book", "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r) DELETE FROM entries WHERE id IN r", ""));
    auto after = J(s.run("book", "INSERT INTO entries (name, message, created_ms) VALUES ('c', 'd', 2)", ""));
    EXPECT_NE(std::string(after.at("error").as_string()).find("rolled back"), std::string::npos) << json::dump(after);
    std::string err;
    EXPECT_FALSE(s.commit(&err));  // so the request fails, and nothing of it lands
  }
  std::string err;
  auto rows = dbs->operate("playground", "guestbook", "SELECT count(*) FROM entries", nullptr, 10, false, &err);
  EXPECT_EQ(json::dump(rows->at("rows")), "[[0]]");
}

TEST_F(DbTest, SchemasOnlyExpand) {
  manifest::TableGrant g;
  g.read_all = true;
  std::vector<manifest::DataStrategy> v1{strategy(g)};
  db::Session(*dbs, "playground", v1, *schemas).run("book", "SELECT 1", "");
  std::string err;
  auto grow = schemas->put(R"({"schema": "guestbook2", "tables": {"entries": {"columns": {
      "name": {"type": "string", "max-length": 20},
      "mood": {"type": "string", "nullable": true}}},
      "replies": {"columns": {"entry_id": {"type": "int64"}, "text": {"type": "string"}}}}})", &err);
  ASSERT_TRUE(grow) << err;
  EXPECT_TRUE(dbs->check("playground", "guestbook", *grow, &err)) << err;
  std::vector<manifest::DataStrategy> v2{strategy(g, "guestbook2")};
  v2[0].tables["replies"] = g;
  db::Session s(*dbs, "playground", v2, *schemas);
  auto r = J(s.run("book", "SELECT mood, message FROM entries", ""));
  EXPECT_FALSE(r.as_object().contains("error")) << json::dump(r);

  auto change = schemas->put(R"({"schema": "guestbook3", "tables": {"entries": {"columns": {
      "name": {"type": "int64"}}}}})", &err);
  ASSERT_TRUE(change);
  EXPECT_FALSE(dbs->check("playground", "guestbook", *change, &err));
  EXPECT_NE(err.find("changes entries.name"), std::string::npos) << err;
  auto must_fill = schemas->put(R"({"schema": "guestbook4", "tables": {"entries": {"columns": {
      "score": {"type": "int64"}}}}})", &err);
  EXPECT_FALSE(dbs->check("playground", "guestbook", *must_fill, &err));
  EXPECT_NE(err.find("nullable or give it a default"), std::string::npos) << err;

  auto d = dbs->describe("playground", "guestbook");
  EXPECT_EQ(d.at("schemas").as_array().size(), 2u);
  EXPECT_EQ(d.at("tables").as_array().size(), 2u);
}

TEST_F(DbTest, OperatorsQueryAndExec) {
  manifest::TableGrant g;
  g.read_all = true;
  g.insert = true;
  std::vector<manifest::DataStrategy> strategies{strategy(g)};
  db::Session s(*dbs, "playground", strategies, *schemas);
  s.run("book", "INSERT INTO entries (name, message, created_ms) VALUES ('Ada', 'hi', 1)", "");
  std::string err;
  ASSERT_TRUE(s.commit(&err)) << err;
  auto q = dbs->operate("playground", "guestbook", "SELECT name FROM entries", nullptr, 100, false, &err);
  ASSERT_TRUE(q) << err;
  EXPECT_EQ(json::dump(q->at("rows")), R"([["Ada"]])");
  EXPECT_FALSE(dbs->operate("playground", "guestbook", "DELETE FROM entries", nullptr, 100, false, &err));
  EXPECT_NE(err.find("read-only"), std::string::npos);
  auto x = dbs->operate("playground", "guestbook", "UPDATE entries SET message = ?", J(R"(["edited"])"), 100, true, &err);
  ASSERT_TRUE(x) << err;
  EXPECT_EQ(x->at("changes"), 1);
  EXPECT_FALSE(dbs->operate("playground", "guestbook", "DELETE FROM goober_schemas", nullptr, 100, true, &err));
  EXPECT_FALSE(dbs->operate("playground", "guestbook", "DROP TABLE entries", nullptr, 100, true, &err));
  EXPECT_FALSE(dbs->operate("playground", "nope", "SELECT 1", nullptr, 100, false, &err));
  // Wide rows of big values are refused before they're built: too many columns, or too many bytes.
  EXPECT_FALSE(dbs->operate("playground", "guestbook", "WITH x(s) AS (SELECT printf('%.4000000c','a')) SELECT s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s,s FROM x",
                            nullptr, 100, false, &err));
  EXPECT_FALSE(dbs->operate("playground", "guestbook", "WITH x(s) AS (SELECT printf('%.4000000c','a')) SELECT s,s,s FROM x",
                            nullptr, 100, false, &err));
  EXPECT_NE(err.find("larger than"), std::string::npos) << err;
  // A query that never ends stops at the operator's step budget.
  EXPECT_FALSE(dbs->operate("playground", "guestbook",
                            "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n) SELECT count(*) FROM n", nullptr,
                            100, false, &err));
  EXPECT_EQ(dbs->tenants(), (std::vector<std::string>{"playground"}));
  EXPECT_EQ(dbs->databases("playground"), (std::vector<std::string>{"guestbook"}));
}

// A box's grants are its whole reach (008 § 4): every way into a table or a column it wasn't
// granted is refused, however the statement gets there.
TEST_F(DbTest, GrantsAreTheWholeReach) {
  std::string err;
  ASSERT_TRUE(schemas->put(R"({"schema": "reach1", "tables": {
    "entries": {"columns": {"name": {"type": "string"}, "message": {"type": "string"}, "approved": {"type": "boolean", "default": false}}},
    "secrets": {"columns": {"token": {"type": "string"}}},
    "tally": {"columns": {"n": {"type": "int64"}}}}})", &err)) << err;
  manifest::DataStrategy owner;
  owner.name = "owner";
  owner.schema = "reach1";
  owner.database = "reach";
  owner.pattern = "sql";
  manifest::TableGrant all;
  all.read_all = all.insert = all.update_all = all.remove = true;
  owner.tables = {{"entries", all}, {"secrets", all}, {"tally", all}};
  manifest::DataStrategy box = owner;
  box.name = "box";
  manifest::TableGrant entries;
  entries.read = {"name", "approved"};
  entries.insert = true;
  entries.update = {"approved"};
  manifest::TableGrant tally;
  tally.insert = true;  // insert only: not even a count
  box.tables = {{"entries", entries}, {"tally", tally}};
  std::vector<manifest::DataStrategy> strategies{owner, box};
  db::Session s(*dbs, "playground", strategies, *schemas);
  ASSERT_FALSE(J(s.run("owner", "INSERT INTO secrets (token) VALUES ('hunter2')", "")).as_object().contains("error"));
  ASSERT_FALSE(J(s.run("owner", "INSERT INTO entries (name, message) VALUES ('a', 'm')", "")).as_object().contains("error"));

  auto refused = [&](const std::string& sql) {
    auto r = J(s.run("box", sql, ""));
    EXPECT_TRUE(r.as_object().contains("error")) << "allowed: " << sql << " -> " << json::dump(r);
  };
  auto allowed = [&](const std::string& sql) {
    auto r = J(s.run("box", sql, ""));
    EXPECT_FALSE(r.as_object().contains("error")) << "refused: " << sql << " -> " << json::dump(r);
  };
  // An ungranted table, every way in.
  refused("SELECT token FROM secrets");
  refused("SELECT * FROM main.secrets");
  refused("SELECT 1 FROM secrets");
  refused("SELECT count(*) FROM secrets");
  refused("SELECT rowid FROM secrets");
  refused("SELECT name FROM entries WHERE EXISTS (SELECT 1 FROM secrets)");
  refused("SELECT (SELECT token FROM secrets) AS t");
  refused("WITH s AS (SELECT token FROM secrets) SELECT * FROM s");
  refused("SELECT name FROM entries UNION SELECT token FROM secrets");
  refused("SELECT name FROM entries JOIN secrets ON 1");
  refused("SELECT * FROM json_each((SELECT group_concat(token) FROM secrets))");
  refused("INSERT INTO entries (name, message) SELECT token, token FROM secrets");
  refused("INSERT INTO entries (name, message) VALUES ((SELECT token FROM secrets), 'x')");
  refused("UPDATE entries SET approved = (SELECT count(*) FROM secrets)");
  refused("DELETE FROM secrets");
  refused("INSERT INTO secrets (token) VALUES ('x')");
  // An insert-only grant reads nothing, not even how many rows there are.
  refused("SELECT count(*) FROM tally");
  refused("SELECT n FROM tally");
  allowed("INSERT INTO tally (n) VALUES (1)");
  // Ungranted columns of a granted table: selected, filtered on, returned, or updated.
  refused("SELECT message FROM entries");
  refused("SELECT * FROM entries");
  refused("SELECT name FROM entries WHERE message = 'm'");
  refused("SELECT name FROM entries ORDER BY message");
  refused("INSERT INTO entries (name, message) VALUES ('b', 'x') RETURNING message");
  refused("UPDATE entries SET message = 'x'");
  // (Without a conflict that can happen, SQLite drops DO UPDATE as unreachable; with one, it asks.)
  refused("INSERT INTO entries (id, name, message) VALUES (1, 'a', 'x') ON CONFLICT (id) DO UPDATE SET message = 'y'");
  refused("INSERT INTO entries (id, name, message) VALUES (1, 'a', 'x') ON CONFLICT DO UPDATE SET message = 'y'");
  refused("DELETE FROM entries");
  allowed("SELECT name, approved FROM entries WHERE approved = 0");
  allowed("UPDATE entries SET approved = 1 WHERE name = 'a'");
  allowed("INSERT INTO entries (name, message) VALUES ('c', 'x') RETURNING name");
  // The schema, the host's tables, and other databases.
  refused("SELECT * FROM sqlite_master");
  refused("SELECT * FROM sqlite_schema");
  refused("SELECT * FROM sqlite_temp_master");
  refused("SELECT * FROM temp.sqlite_master");
  refused("SELECT * FROM pragma_table_info('secrets')");
  refused("SELECT * FROM goober_schemas");
  refused("SELECT * FROM goober_columns");
  refused("PRAGMA table_list");
  refused("ATTACH ':memory:' AS other");
  refused("CREATE TEMP VIEW v AS SELECT token FROM secrets");
  refused("CREATE TEMP TABLE t (x)");
  refused("CREATE TEMP TRIGGER tr AFTER INSERT ON entries BEGIN DELETE FROM secrets; END");
  refused("CREATE VIEW v AS SELECT token FROM secrets");
  // Whatever was refused, nothing reached the ungranted table.
  auto left = J(s.run("owner", "SELECT token FROM secrets", ""));
  EXPECT_EQ(json::dump(left.at("rows")), R"([["hunter2"]])");

  // What the box used is what ran: refused statements count for nothing.
  const auto& used = s.used().at("box");
  EXPECT_EQ(used.at("entries").read, (std::set<std::string>{"approved", "name"}));
  EXPECT_EQ(used.at("entries").update, (std::set<std::string>{"approved"}));
  EXPECT_TRUE(used.at("entries").insert);
  EXPECT_FALSE(used.at("entries").remove);
  EXPECT_TRUE(used.at("tally").insert);
  EXPECT_TRUE(used.at("tally").read.empty());
  EXPECT_FALSE(used.count("secrets"));
}
