goober_package(
  NAME host
  SOURCES src/worker_pool.cpp src/rate_limit.cpp src/mailer.cpp src/backup.cpp src/backups.cpp src/rehydrate.cpp src/scheduler.cpp src/keys.cpp src/network.cpp src/outbox.cpp src/host.cpp src/operations.cpp src/identity_routes.cpp src/oauth_routes.cpp src/mcp_routes.cpp src/operator_tokens.cpp
          src/attachment_routes.cpp src/content_tools.cpp src/data_tools.cpp src/web_fetch.cpp src/crawler.cpp
  DEPS goober_base goober_aws goober_backup goober_json goober_config goober_wal goober_content goober_site goober_attachments goober_router goober_manifest
       goober_runtime_wasm goober_db goober_boxes goober_identity goober_server_http goober_mcp goober_timers goober_web
  TESTS test/host_test.cpp test/identity_routes_test.cpp test/operator_tokens_test.cpp test/content_test.cpp test/data_test.cpp
        test/oauth_routes_test.cpp test/agents_test.cpp test/rate_limit_test.cpp test/mailer_test.cpp test/backup_test.cpp test/lifecycle_test.cpp test/timers_test.cpp test/network_test.cpp test/internal_test.cpp test/http_contract_test.cpp test/web_test.cpp test/mail_test.cpp test/s3_backup_test.cpp
)

add_executable(goober-host src/main.cpp)
target_link_libraries(goober-host PRIVATE goober_host)
target_compile_options(goober-host PRIVATE -Wall -Wextra -Wpedantic)

# The Rust binding for boxes, embedded so the rust_binding tool can hand it to a remote agent.
file(READ "${CMAKE_SOURCE_DIR}/examples/boxes/shared/goober.rs" GOOBER_RUST_BINDING)
configure_file(src/rust_binding.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/rust_binding.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/examples/boxes/shared/goober.rs")
target_include_directories(goober_host PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
