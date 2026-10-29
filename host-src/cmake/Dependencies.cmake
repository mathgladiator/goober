# Third-party dependencies live under ~/.local (see tools/install-deps.sh).
set(GOOBER_LOCAL "$ENV{HOME}/.local" CACHE PATH "Prefix holding third-party dependencies")
set(GOOBER_WASMTIME_VERSION "v49.0.4" CACHE STRING "Pinned wasmtime release")
set(GOOBER_WASMTIME_DIR "${GOOBER_LOCAL}/opt/wasmtime-${GOOBER_WASMTIME_VERSION}-x86_64-linux-c-api"
    CACHE PATH "wasmtime C API directory")

list(PREPEND CMAKE_PREFIX_PATH "${GOOBER_LOCAL}")

find_package(Threads REQUIRED)
if(BUILD_TESTING)
  find_package(GTest REQUIRED CONFIG)
endif()

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

# SQLite: the static amalgamation under ~/.local, for the identity service's realms.
find_path(GOOBER_SQLITE_INCLUDE sqlite3.h HINTS "${GOOBER_LOCAL}/include" REQUIRED)
find_library(GOOBER_SQLITE_LIB NAMES libsqlite3.a sqlite3 HINTS "${GOOBER_LOCAL}/lib" REQUIRED)
add_library(goober_sqlite3 INTERFACE)
target_include_directories(goober_sqlite3 SYSTEM INTERFACE "${GOOBER_SQLITE_INCLUDE}")
target_link_libraries(goober_sqlite3 INTERFACE "${GOOBER_SQLITE_LIB}" Threads::Threads ${CMAKE_DL_LIBS} m)

# OpenSSL's libcrypto, static and pinned under ~/.local/opt, for Web Push (P-256: ES256 for
# VAPID, ECDH and AES-128-GCM for payloads; design/machine/782.PWA.md). Only webpush uses it.
set(GOOBER_OPENSSL_VERSION "3.5.1" CACHE STRING "Pinned OpenSSL release")
set(GOOBER_OPENSSL_DIR "${GOOBER_LOCAL}/opt/openssl-${GOOBER_OPENSSL_VERSION}" CACHE PATH "OpenSSL directory")
if(NOT EXISTS "${GOOBER_OPENSSL_DIR}/lib/libcrypto.a")
  message(FATAL_ERROR "OpenSSL not found at ${GOOBER_OPENSSL_DIR}; run tools/install-deps.sh")
endif()
add_library(goober_libcrypto STATIC IMPORTED GLOBAL)
set_target_properties(goober_libcrypto PROPERTIES
  IMPORTED_LOCATION "${GOOBER_OPENSSL_DIR}/lib/libcrypto.a"
  INTERFACE_INCLUDE_DIRECTORIES "${GOOBER_OPENSSL_DIR}/include"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GOOBER_OPENSSL_DIR}/include")
target_link_libraries(goober_libcrypto INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
