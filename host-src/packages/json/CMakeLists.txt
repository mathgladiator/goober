goober_package(
  NAME json
  SOURCES src/boost_json.cpp src/json.cpp
  DEPS goober_boost
  TESTS test/json_test.cpp
)
