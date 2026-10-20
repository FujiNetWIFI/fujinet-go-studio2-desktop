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
  src/debug.c)

# The embedded ROM table: empty unless WITH_STUDIO2_ROMS (see COMPLIANCE.md).
set(STUDIO2_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(WITH_STUDIO2_ROMS)
  file(GLOB STUDIO2_ROM_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/*")
  add_custom_command(
    OUTPUT "${STUDIO2_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            "${STUDIO2_ROMS_C}" "${CMAKE_SOURCE_DIR}/tools/roms"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py" ${STUDIO2_ROM_INPUTS}
    COMMENT "Embedding the BIOS images in tools/roms (WITH_STUDIO2_ROMS=ON: NOT redistributable)"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${STUDIO2_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            --none "${STUDIO2_ROMS_C}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
    VERBATIM)
endif()

add_library(studio2_session STATIC ${STUDIO2_SESSION_SOURCES} "${STUDIO2_ROMS_C}")
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
