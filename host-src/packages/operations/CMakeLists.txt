goober_package(
  NAME operations
  SOURCES src/traffic.cpp src/changes.cpp
  DEPS goober_sqlite goober_wal goober_json goober_base
  TESTS test/traffic_test.cpp test/changes_test.cpp
)
