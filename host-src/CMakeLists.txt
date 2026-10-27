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
foreach(pkg base json web types timers render sqlite config wal content attachments site router manifest runtime_wasm db boxes identity server_http mcp host fuzz)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packages/${pkg}/CMakeLists.txt")
    add_subdirectory(packages/${pkg})
  endif()
endforeach()
