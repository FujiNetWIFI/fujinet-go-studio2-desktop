# studio2_session: the frontend contract (core/include/studio2session.h), included
# from core/CMakeLists.txt once SDL3 is found.

set(STUDIO2_SESSION_SOURCES
  src/session.c
  src/settings.c
  src/paths.c
  src/media.c
  src/roms.c
  src/fujinet_runtime.c
  src/audio_sdl.c
  src/bindings.c
  src/hid_keys.c
  src/gamepad_sdl.c
  src/debug.c
  src/help.c)

# The embedded BIOS table (tools/roms/embed-roms.py). The RCA Studio II BIOS is
# in the public domain (COMPLIANCE.md), so the frontends link the full table
# (studio2_bios_full). The tests link an empty one (studio2_bios_none): their
# goldens run a BIOS of their own and the stand-in, and must not see RCA's.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
file(GLOB STUDIO2_ROM_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/*")
set(STUDIO2_BIOS_FULL_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded_full.c")
set(STUDIO2_BIOS_NONE_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded_none.c")
add_custom_command(
  OUTPUT "${STUDIO2_BIOS_FULL_C}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
          "${STUDIO2_BIOS_FULL_C}" "${CMAKE_SOURCE_DIR}/tools/roms"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py" ${STUDIO2_ROM_INPUTS}
  COMMENT "Embedding the BIOS"
  VERBATIM)
add_custom_command(
  OUTPUT "${STUDIO2_BIOS_NONE_C}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
          --none "${STUDIO2_BIOS_NONE_C}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
  VERBATIM)
foreach(_v full none)
  string(TOUPPER ${_v} _V)
  add_library(studio2_bios_${_v} STATIC "${STUDIO2_BIOS_${_V}_C}")
  set_target_properties(studio2_bios_${_v} PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
  target_include_directories(studio2_bios_${_v} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endforeach()

# The Help, embedded: data/help/help.md as topics of styled spans
# (core/include/studio2help.h), which every frontend lays out with its own
# toolkit. help_data.h declares the table; the build generates it.
set(STUDIO2_HELP_C "${CMAKE_BINARY_DIR}/generated/help_content.c")
add_custom_command(
  OUTPUT "${STUDIO2_HELP_C}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/help/make-help.py"
          --src "${CMAKE_SOURCE_DIR}/data/help/help.md"
          --out-c "${STUDIO2_HELP_C}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/help/make-help.py"
          "${CMAKE_SOURCE_DIR}/data/help/help.md"
  COMMENT "Help"
  VERBATIM)

add_library(studio2_session STATIC ${STUDIO2_SESSION_SOURCES} "${STUDIO2_HELP_C}")
set_target_properties(studio2_session PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(studio2_session PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(studio2_session PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/debugger")
target_link_libraries(studio2_session PUBLIC studio2_core SDL3::SDL3)
# setenv()/getaddrinfo() under a strict -std=c11
target_compile_definitions(studio2_session PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
set_property(SOURCE ${STUDIO2_SESSION_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>"
  # paths are bounded by STUDIO2_PATH_MAX and joined with snprintf on purpose;
  # GCC's truncation analysis flags every such join
  "$<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>")

# Where an uninstalled build and an installed copy find the FujiNet runtime
# (core/src/paths.c probes these after the executable's own directory).
target_compile_definitions(studio2_session PRIVATE
  STUDIO2_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  STUDIO2_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-studio2"
  STUDIO2_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-studio2")
if(WIN32)
  target_link_libraries(studio2_session PUBLIC ws2_32)
else()
  target_link_libraries(studio2_session PUBLIC ${CMAKE_DL_LIBS})
endif()
