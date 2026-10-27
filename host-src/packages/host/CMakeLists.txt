goober_package(
  NAME host
  SOURCES src/worker_pool.cpp src/rate_limit.cpp src/mailer.cpp src/host.cpp src/operations.cpp src/identity_routes.cpp src/oauth_routes.cpp src/mcp_routes.cpp src/operator_tokens.cpp
          src/attachment_routes.cpp src/content_tools.cpp src/data_tools.cpp
  DEPS goober_base goober_json goober_config goober_wal goober_content goober_site goober_attachments goober_router goober_manifest
       goober_runtime_wasm goober_db goober_boxes goober_identity goober_server_http goober_mcp
  TESTS test/host_test.cpp test/identity_routes_test.cpp test/operator_tokens_test.cpp test/content_test.cpp test/data_test.cpp
        test/oauth_routes_test.cpp test/agents_test.cpp test/rate_limit_test.cpp test/mailer_test.cpp
)

add_executable(goober-host src/main.cpp)
target_link_libraries(goober-host PRIVATE goober_host)
target_compile_options(goober-host PRIVATE -Wall -Wextra -Wpedantic)
