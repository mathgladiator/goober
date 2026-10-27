goober_package(
  NAME web
  SOURCES src/url.cpp src/robots.cpp src/html.cpp
  DEPS goober_base goober_json
  TESTS test/url_test.cpp test/robots_test.cpp test/html_test.cpp
)
