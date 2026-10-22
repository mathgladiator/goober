# Third-party dependencies live under ~/.local (see tools/install-deps.sh).
set(GOOBER_LOCAL "$ENV{HOME}/.local" CACHE PATH "Prefix holding third-party dependencies")
set(GOOBER_WASMTIME_VERSION "v49.0.4" CACHE STRING "Pinned wasmtime release")
set(GOOBER_WASMTIME_DIR "${GOOBER_LOCAL}/opt/wasmtime-${GOOBER_WASMTIME_VERSION}-x86_64-linux-c-api"
    CACHE PATH "wasmtime C API directory")

list(PREPEND CMAKE_PREFIX_PATH "${GOOBER_LOCAL}")

find_package(Threads REQUIRED)
find_package(GTest REQUIRED CONFIG)

# Boost: header-only use of Asio, Beast, and JSON (JSON is compiled in packages/json).
find_path(GOOBER_BOOST_INCLUDE boost/version.hpp HINTS "${GOOBER_LOCAL}/include" REQUIRED)
add_library(goober_boost INTERFACE)
target_include_directories(goober_boost SYSTEM INTERFACE "${GOOBER_BOOST_INCLUDE}")
target_compile_definitions(goober_boost INTERFACE BOOST_ASIO_NO_DEPRECATED BOOST_JSON_NO_LIB BOOST_CONTAINER_NO_LIB)
target_link_libraries(goober_boost INTERFACE Threads::Threads)

# wasmtime: static C library with the header-only C++ wrapper.
if(NOT EXISTS "${GOOBER_WASMTIME_DIR}/lib/libwasmtime.a")
  message(FATAL_ERROR "wasmtime not found at ${GOOBER_WASMTIME_DIR}; run tools/install-deps.sh")
endif()
add_library(goober_wasmtime STATIC IMPORTED GLOBAL)
set_target_properties(goober_wasmtime PROPERTIES
  IMPORTED_LOCATION "${GOOBER_WASMTIME_DIR}/lib/libwasmtime.a"
  INTERFACE_INCLUDE_DIRECTORIES "${GOOBER_WASMTIME_DIR}/include"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GOOBER_WASMTIME_DIR}/include")
target_link_libraries(goober_wasmtime INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
