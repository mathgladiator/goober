goober_package(
  NAME logs
  SOURCES src/logs.cpp src/search.cpp
  DEPS goober_base goober_json
  TESTS test/logs_test.cpp
)
