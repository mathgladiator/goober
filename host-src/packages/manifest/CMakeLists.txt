goober_package(
  NAME manifest
  SOURCES src/manifest.cpp
  DEPS goober_base goober_json goober_types goober_timers goober_render goober_router goober_mail_inbound
  TESTS test/manifest_test.cpp
)
