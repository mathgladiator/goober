goober_package(
  NAME libraries
  SOURCES src/registry.cpp src/common.cpp src/time_lib.cpp src/ics_lib.cpp
  DEPS goober_calendar goober_json goober_base
  TESTS test/time_test.cpp test/ics_test.cpp
)
