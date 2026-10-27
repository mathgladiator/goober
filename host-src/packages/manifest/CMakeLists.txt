goober_package(
  NAME manifest
  SOURCES src/manifest.cpp
  DEPS goober_base goober_json goober_render goober_router
  TESTS test/manifest_test.cpp
)
