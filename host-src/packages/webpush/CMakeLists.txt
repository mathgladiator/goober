goober_package(
  NAME webpush
  SOURCES src/webpush.cpp
  DEPS goober_base goober_libcrypto
  TESTS test/webpush_test.cpp
)
