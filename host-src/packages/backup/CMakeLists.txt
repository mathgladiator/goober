goober_package(
  NAME backup
  SOURCES src/seal.cpp src/pages.cpp src/names.cpp
  DEPS goober_base goober_json
  TESTS test/backup_test.cpp
)
