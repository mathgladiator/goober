goober_package(
  NAME server_http
  SOURCES src/server.cpp src/multipart.cpp
  DEPS goober_boost goober_base
  TESTS test/server_test.cpp
)
