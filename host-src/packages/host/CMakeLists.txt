goober_package(
  NAME host
  SOURCES src/worker_pool.cpp src/rate_limit.cpp src/mailer.cpp src/backup.cpp src/backups.cpp src/rehydrate.cpp src/scheduler.cpp src/identity_events.cpp src/mail_tools.cpp src/mail_in.cpp src/mail_out.cpp src/toolchain.cpp src/keys.cpp src/network.cpp src/outbox.cpp src/host.cpp src/operations.cpp src/identity_routes.cpp src/oauth_routes.cpp src/mcp_routes.cpp src/operator_tokens.cpp
          src/attachment_routes.cpp src/content_tools.cpp src/data_tools.cpp src/web_fetch.cpp src/crawler.cpp src/builder.cpp src/source_tools.cpp src/panel.cpp src/pwa_routes.cpp src/operator_scope.cpp src/consent.cpp src/invites.cpp src/logs_tools.cpp src/http_out.cpp src/connect_routes.cpp src/approvals.cpp src/sse.cpp src/audit.cpp src/grant_usage.cpp src/jobs.cpp src/route_cache.cpp src/cache_routes.cpp src/flags.cpp src/db_admin.cpp
          src/dns.cpp src/mail_check.cpp
  DEPS goober_base goober_aws goober_backup goober_json goober_config goober_wal goober_content goober_site goober_attachments goober_router goober_manifest
       goober_runtime_wasm goober_db goober_boxes goober_identity goober_mail_templates goober_mail_inbound goober_server_http goober_mcp goober_timers goober_web goober_source goober_operations goober_panel goober_webpush goober_calendar goober_libraries goober_mail_consent goober_logs goober_vault goober_specs goober_signing goober_bus goober_ids
  TESTS test/host_test.cpp test/identity_routes_test.cpp test/operator_tokens_test.cpp test/content_test.cpp test/data_test.cpp
        test/oauth_routes_test.cpp test/agents_test.cpp test/rate_limit_test.cpp test/mailer_test.cpp test/backup_test.cpp test/lifecycle_test.cpp test/timers_test.cpp test/identity_events_test.cpp test/operator_oauth_test.cpp test/mail_in_test.cpp test/network_test.cpp test/internal_test.cpp test/http_contract_test.cpp test/web_test.cpp test/mail_test.cpp test/s3_backup_test.cpp test/source_tools_test.cpp test/panel_test.cpp test/pwa_test.cpp test/libraries_test.cpp test/operator_scope_test.cpp test/invites_test.cpp test/logs_host_test.cpp test/http_test.cpp test/bus_test.cpp test/audit_test.cpp test/cache_test.cpp
        test/mail_check_test.cpp
)
# MX and TXT lookups (dns.cpp) use glibc's resolver.
target_link_libraries(goober_host PUBLIC resolv)

add_executable(goober-host src/main.cpp)
target_link_libraries(goober-host PRIVATE goober_host)
target_compile_options(goober-host PRIVATE -Wall -Wextra -Wpedantic)

# The Rust binding for boxes, embedded so the rust_binding tool can hand it to a remote agent.
file(READ "${CMAKE_SOURCE_DIR}/examples/boxes/shared/goober.rs" GOOBER_RUST_BINDING)
configure_file(src/rust_binding.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/rust_binding.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/examples/boxes/shared/goober.rs")
target_include_directories(goober_host PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# The commit each build came from (host_health, goober-host --version), checked on every build.
add_custom_target(goober_build_id
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/build_id.h
          -P ${CMAKE_SOURCE_DIR}/cmake/BuildId.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/build_id.h
  COMMENT "build id")
add_dependencies(goober_host goober_build_id)
add_dependencies(goober-host goober_build_id)
target_include_directories(goober-host PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
