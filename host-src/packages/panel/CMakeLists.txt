# The panel's browser side (design/machine/028.Panel.md § 3), compiled into the binary.
file(GLOB PANEL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
set(PANEL_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/panel_assets.cpp")
add_custom_command(
  OUTPUT "${PANEL_OUT}"
  COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/assets -DOUT=${PANEL_OUT} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Embed.cmake
  DEPENDS ${PANEL_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Embed.cmake
  COMMENT "panel assets")
goober_package(
  NAME panel
  SOURCES "${PANEL_OUT}"
  TESTS test/panel_test.cpp
)
