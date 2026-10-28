goober_package(
  NAME aws
  SOURCES src/sigv4.cpp src/request.cpp src/s3.cpp src/ses.cpp
  DEPS goober_base goober_json
  TESTS test/aws_test.cpp
  TEST_DEPS goober_server_http
)
