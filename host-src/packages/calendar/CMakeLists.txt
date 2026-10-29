goober_package(
  NAME calendar
  SOURCES src/zones.cpp src/rrule.cpp src/ics.cpp
  DEPS goober_base
  TESTS test/rrule_test.cpp
)
