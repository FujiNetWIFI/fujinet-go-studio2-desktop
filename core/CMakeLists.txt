# studio2_core: the toolkit-agnostic, SDL-free RCA Studio II core.
#
#   studio2/            the emulator: MAME's Studio II driver, its CDP1802
#                       and CDP1861 and its beeper transposed to C
#                       (machine.c, cpu1802.c, cdp1861.c), the no-BIOS
#                       stand-in (stub.asm / stub.c), the paced host thread,
#                       and the FujiNet cartridge device.
#
#   studio2/fuji-generated/ the cartridge firmware's OWN sources -- s2map.c
#                       (the image planner and page view), fuji_load.c
#                       (staging and the swap), s2_text.c / s2_font.c (the
#                       text engine), s2_cart.h (the bus decision and the
#                       raster served by DMA timing), fujimail.c (the
#                       mailbox), fujibus.c (the SLIP + FujiBus wire codec)
#                       and the baked CONFIG client -- staged by
#                       cmake/StageFujiProto.cmake. Compiling these rather
#                       than reimplementing them is what makes the emulated
#                       cartridge and the real RP2354B one identical by
#                       construction.
#
#   debugger/           the debugger engine (core/include/studio2debug.h).
#
# The one port-specific cartridge file is studio2/fujinet_cart.c, transposed
# from the firmware's MAME device (pico/studio2/emu/studio2_fujinet.cpp). The
# transport is studio2/fujitcp_host.c, this app's own portable twin of the
# staged fujitcp.c (see its header).
#
# Nothing here links SDL. The session layer (SDL3 audio + gamepad) is a
# separate target so that stays true; no_sdl_link guards it.

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/studio2/fuji-generated")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h")
  message(FATAL_ERROR
    "Staged FujiNet sources are missing. cmake/StageFujiProto.cmake should "
    "have produced them; re-run with -DFUJI_RESTAGE=ON.")
endif()

# The staged cartridge sources (the staged fujitcp.c is not compiled; see
# studio2/fujitcp_host.c).
set(STUDIO2_FUJI_SOURCES
  ${FUJI_GEN}/fujimail.c
  ${FUJI_GEN}/fujibus.c
  ${FUJI_GEN}/fuji_crc.c
  ${FUJI_GEN}/s2map.c
  ${FUJI_GEN}/fuji_load.c
  ${FUJI_GEN}/s2_text.c
  ${FUJI_GEN}/s2_font.c)

set(STUDIO2_EMU_SOURCES
  studio2/cpu1802.c
  studio2/cdp1861.c
  studio2/machine.c
  studio2/stub.c
  studio2/dasm1802.c
  studio2/fujinet_cart.c
  studio2/fujitcp_host.c
  studio2/host.c)

set(STUDIO2_DEBUGGER_SOURCES)
foreach(_f debugger.c expr.c fujitrace.c prompt.c symbols.c symbols_builtin.c rasterview.c)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/debugger/${_f}")
    list(APPEND STUDIO2_DEBUGGER_SOURCES debugger/${_f})
  endif()
endforeach()

add_library(studio2_core STATIC
  ${STUDIO2_EMU_SOURCES} ${STUDIO2_FUJI_SOURCES} ${STUDIO2_DEBUGGER_SOURCES})
set_target_properties(studio2_core PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(studio2_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/studio2"
  # public because the debugger and the tests read the cart's state through
  # the staged fuji_mailbox.h, s2map.h and s2_text.h
  "${FUJI_GEN}")
target_include_directories(studio2_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/debugger")

# getaddrinfo/clock_gettime/nanosleep under a strict -std=c11.
target_compile_definitions(studio2_core PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)

target_link_libraries(studio2_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(studio2_core PUBLIC ws2_32)
else()
  target_link_libraries(studio2_core PUBLIC m)
endif()

# Our own modules are warning-clean; the staged tree is built with whatever
# warnings upstream is comfortable with.
set(STUDIO2_WARN_SOURCES ${STUDIO2_EMU_SOURCES} ${STUDIO2_DEBUGGER_SOURCES})
list(REMOVE_ITEM STUDIO2_WARN_SOURCES debugger/symbols_builtin.c)
set_property(SOURCE ${STUDIO2_WARN_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>")

# ---- studio2_session: the frontend contract ----------------------------------
#
# A separate target from studio2_core so the core stays SDL-free: audio_sdl.c and
# gamepad_sdl.c live here. Found-or-skipped, exactly as the frontends are:
# `-DFRONTEND=none` on a machine (or a cross-toolchain) with no SDL3 must
# still configure and build the emulator and its tests.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/session.c")
  find_package(SDL3 CONFIG QUIET)
  if(NOT SDL3_FOUND)
    message(STATUS "studio2_session: SDL3 not found -- building the core and its "
                   "tests only (no audio, no gamepads, no frontends)")
  else()
    include(session.cmake)
  endif()
endif()

add_subdirectory(tests)
