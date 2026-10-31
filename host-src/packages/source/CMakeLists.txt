goober_package(
  NAME source
  SOURCES src/store.cpp src/diff.cpp src/patch.cpp
  DEPS goober_base goober_json goober_sqlite
  TESTS test/source_test.cpp
)
