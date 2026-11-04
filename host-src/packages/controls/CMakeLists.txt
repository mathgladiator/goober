goober_package(
  NAME controls
  SOURCES src/spec.cpp src/gate.cpp src/report.cpp src/queue.cpp
  DEPS goober_operations goober_json goober_base
  TESTS test/spec_test.cpp test/gate_test.cpp test/report_test.cpp test/queue_test.cpp
)
