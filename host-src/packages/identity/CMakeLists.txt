goober_package(
  NAME identity
  SOURCES src/identity.cpp
  DEPS goober_base goober_json goober_sqlite
  TESTS test/identity_test.cpp
)
