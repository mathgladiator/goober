goober_package(
  NAME mail_templates
  SOURCES src/defaults.cpp src/templates.cpp
  DEPS goober_base goober_json goober_render
  TESTS test/templates_test.cpp
)
