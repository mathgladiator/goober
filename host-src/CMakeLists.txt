cmake_minimum_required(VERSION 3.25)
project(goober_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(cmake/Dependencies.cmake)
include(cmake/GooberPackage.cmake)

# Packages, in dependency order. Each is one directory with its own target and tests.
foreach(pkg base bus json calendar aws backup web types timers render mail_templates mail_inbound sqlite ids mail_consent logs vault specs signing webpush libraries config wal operations content attachments site router manifest runtime_wasm db boxes identity server_http mcp source panel host fuzz)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packages/${pkg}/CMakeLists.txt")
    add_subdirectory(packages/${pkg})
  endif()
endforeach()

# The deploy's own edits of host.json and the Caddyfile for a new tenant (tools/tenant_edit.py),
# each edited host.json checked by the host itself.
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME tools.tenant_edit
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/tenant_edit_test.py $<TARGET_FILE:goober-host>)
  endif()
endif()
