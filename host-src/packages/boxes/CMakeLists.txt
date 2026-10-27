goober_package(
  NAME boxes
  SOURCES src/registry.cpp
  DEPS goober_base goober_wal goober_types goober_manifest goober_router goober_runtime_wasm
  TESTS test/registry_test.cpp
)
