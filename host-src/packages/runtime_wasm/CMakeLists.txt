goober_package(
  NAME runtime_wasm
  SOURCES src/engine.cpp
  DEPS goober_base goober_wasmtime
  TESTS test/engine_test.cpp
)
