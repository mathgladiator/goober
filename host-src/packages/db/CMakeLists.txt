goober_package(
  NAME db
  SOURCES src/schema.cpp src/database.cpp
  DEPS goober_base goober_json goober_wal goober_sqlite goober_manifest
  TESTS test/db_test.cpp
)
