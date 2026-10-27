goober_package(
  NAME site
  SOURCES src/site.cpp src/builtin.cpp
  DEPS goober_content goober_render goober_json goober_base
  TESTS test/site_test.cpp
)
