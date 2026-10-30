goober_package(
  NAME signing
  SOURCES src/oauth1.cpp
  DEPS goober_base goober_libcrypto
  TESTS test/oauth1_test.cpp
)
