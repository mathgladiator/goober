goober_package(
  NAME operations
  SOURCES src/traffic.cpp src/changes.cpp src/live.cpp src/failures.cpp
  DEPS goober_sqlite goober_wal goober_json goober_base
  TESTS test/traffic_test.cpp test/changes_test.cpp test/live_test.cpp test/failures_test.cpp
)
