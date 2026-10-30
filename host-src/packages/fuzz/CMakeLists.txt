goober_package(
  NAME fuzz
  TESTS test/fuzz_test.cpp test/sql_property_test.cpp
  TEST_DEPS goober_web goober_server_http goober_render goober_types goober_manifest goober_router goober_timers goober_identity goober_mcp goober_db
            goober_json goober_wal goober_source goober_backup goober_calendar goober_libraries goober_specs goober_site goober_vault goober_signing
)
