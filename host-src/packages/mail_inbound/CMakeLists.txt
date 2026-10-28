goober_package(
  NAME mail_inbound
  SOURCES src/address.cpp src/mime.cpp
  DEPS goober_base goober_json goober_render
  TESTS test/address_test.cpp test/mime_test.cpp
)
