goober_package(
  NAME attachments
  SOURCES src/attachments.cpp
  DEPS goober_base goober_wal goober_content goober_sqlite
  TESTS test/attachments_test.cpp
)
