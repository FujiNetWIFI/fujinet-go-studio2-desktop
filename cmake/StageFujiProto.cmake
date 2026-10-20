# Provide the fujinet-firmware checkout (see cmake/Dependencies.cmake) and
# stage the FujiNet Studio II cartridge's own sources into
# core/studio2/fuji-generated, which is what core/CMakeLists.txt compiles into
# the emulator core.
#
# These are the cartridge firmware's OWN sources, from two roots:
#
#   pico/studio2/firmware  s2map.c (the image planner and the page view: which
#                          of the 256 pages the cart claims, and as what),
#                          fuji_load.c (ping-pong staging, the swap), s2_text.c
#                          + s2_font.c (the 16x10 text engine drawn into the
#                          cart-served raster), s2_cart.h (the bus decode and
#                          the raster served by DMA timing), fuji_mailbox.h
#                          (the arena layout) and the baked CONFIG client
#                          (fujiconfigrom.h)
#   pico/common            fujimail.c (hotspot decode, the REGSEL/REGDATA
#                          interlock, ACKSEQ published last, the DBC push
#                          receiver), fujibus.c (SLIP + FujiBus wire codec),
#                          the shared mailbox protocol headers and the
#                          emulator-side SLIP-over-TCP transport (fujitcp)
#
# The firmware's pico/studio2/emu/apply.sh grafts the identical set into a
# MAME tree; this file is that graft for this repository. Emulator and
# cartridge stay identical by construction, not by discipline: nothing here is
# patched, only copied. The one port-specific file, the cart device itself, is
# this repository's own core/studio2/fujinet_cart.c (transposed from the
# firmware's MAME device, pico/studio2/emu/studio2_fujinet.cpp).
#
# apply.sh additionally generates a fujins.h that prefixes every shared
# symbol, because several FujiNet carts share one MAME binary; it also renames
# every .c to .cpp for MAME's precompiled header. Only one FujiNet cart is ever
# linked here and the sources are C, so neither is reproduced. Do not add them
# "for symmetry".
#
# The staged fujitcp.c is POSIX sockets only and is not compiled: every build
# uses core/studio2/fujitcp_host.c, this app's portable, abortable twin,
# against the SAME staged fujitcp.h. The staged .c is still copied so the
# staging result does not depend on the host.
#
# Staging is automatic: it runs when the staged tree is missing, when the pin
# has moved, when this file changes, or on demand with -DFUJI_RESTAGE=ON
# (which is also how to pick up uncommitted edits in a working checkout
# pointed at by FUJINET_SRC).

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/studio2/fuji-generated")

option(FUJI_RESTAGE "Re-stage the FujiNet protocol sources from the firmware checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

studio2_provide_dependency(
  NAME fujinet-firmware
  PATH third_party/fujinet-firmware
  URL "${FUJINET_URL}"
  COMMIT "${FUJINET_COMMIT}"
  SENTINEL build.sh
  OVERRIDE FUJINET_SRC
  RESULT FUJINET_DIR)

set(FUJI_PROTO_DIR "${FUJINET_DIR}/pico")
if(NOT EXISTS "${FUJI_PROTO_DIR}/studio2/firmware/include/fuji_mailbox.h"
   OR NOT EXISTS "${FUJI_PROTO_DIR}/common/include/fujimail.h")
  message(FATAL_ERROR
    "${FUJINET_DIR} has no pico/studio2/firmware/include/fuji_mailbox.h or "
    "pico/common/include/fujimail.h -- the pin must be on master at or after "
    "#2083; see cmake/Dependencies.cmake.")
endif()

# The exact file list apply.sh grafts into MAME, relative to pico/. The CONFIG
# client comes from firmware/baked/ -- apply.sh prefers a fresh build/gen when
# a developer has run build-cart.sh, but a pinned checkout never has one, and
# baked/ is what the cartridge firmware itself ships.
set(FUJI_STAGE_FILES
  studio2/firmware/src/s2map.c
  studio2/firmware/src/fuji_load.c
  studio2/firmware/src/s2_text.c
  studio2/firmware/src/s2_font.c
  studio2/firmware/include/s2map.h
  studio2/firmware/include/s2_cart.h
  studio2/firmware/include/s2_text.h
  studio2/firmware/include/fuji_load.h
  studio2/firmware/include/fuji_mailbox.h
  studio2/firmware/baked/fujiconfigrom.h
  common/src/fujimail.c
  common/src/fujibus.c
  common/include/fujimail.h
  common/include/fujibus.h
  common/include/fuji_mailbox_proto.h
  common/include/fuji_mailbox_check.h
  common/emu/fujitcp.c
  common/emu/fujitcp.h)

# What the staged tree was made from: the source identity plus a hash of this
# file (the only transform), so that editing the stage re-stages rather than
# silently leaving the old result in place.
set(_fuji_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${FUJINET_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _fuji_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _fuji_head)
  set(_fuji_head "${FUJINET_COMMIT} ${FUJINET_DIR}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _fuji_stage_hash)
set(_fuji_want "${_fuji_head} ${_fuji_stage_hash}")

set(_fuji_staged "")
if(EXISTS "${FUJI_GEN}/.source-info")
  file(READ "${FUJI_GEN}/.source-info" _fuji_staged)
  string(STRIP "${_fuji_staged}" _fuji_staged)
endif()

if(FUJI_RESTAGE OR NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h"
   OR NOT _fuji_staged STREQUAL _fuji_want)
  message(STATUS "Staging FujiNet protocol sources from ${FUJI_PROTO_DIR}")
  file(REMOVE_RECURSE "${FUJI_GEN}")
  file(MAKE_DIRECTORY "${FUJI_GEN}")

  foreach(_f IN LISTS FUJI_STAGE_FILES)
    if(NOT EXISTS "${FUJI_PROTO_DIR}/${_f}")
      file(REMOVE_RECURSE "${FUJI_GEN}")
      message(FATAL_ERROR
        "FujiNet protocol staging failed: ${FUJI_PROTO_DIR}/${_f} is missing "
        "(pin drift? see cmake/Dependencies.cmake).")
    endif()
    get_filename_component(_base "${_f}" NAME)
    file(COPY_FILE "${FUJI_PROTO_DIR}/${_f}" "${FUJI_GEN}/${_base}")
  endforeach()

  file(WRITE "${FUJI_GEN}/.source-info" "${_fuji_want}\n")
endif()

message(STATUS "FujiNet protocol sources staged from ${FUJI_PROTO_DIR}")
