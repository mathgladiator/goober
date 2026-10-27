goober_package(
  NAME sqlite
  SOURCES src/sqlite.cpp
  DEPS goober_sqlite3
  TESTS test/sqlite_test.cpp
)
