goober_package(
  NAME identity
  SOURCES src/identity.cpp src/sqlite.cpp
  DEPS goober_base goober_json goober_sqlite3
  TESTS test/identity_test.cpp
)
