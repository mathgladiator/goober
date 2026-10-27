goober_package(
  NAME render
  SOURCES src/frontmatter.cpp src/mustache.cpp src/markdown.cpp
  DEPS goober_base goober_json
  TESTS test/frontmatter_test.cpp test/mustache_test.cpp test/markdown_test.cpp
)
