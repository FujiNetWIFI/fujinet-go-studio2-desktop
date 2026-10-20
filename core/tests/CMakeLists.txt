# core tests (see each file's header). Network and ROM-dependent tests exit
# 77, which ctest reports as skipped.

function(studio2_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE studio2_core)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/debugger")
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE
    STUDIO2_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  # The debugger contract: attach stops the machine, registers, every step
# kind, disassembly, breakpoints of every class with conditions, memory, the
# video views, I/O, the cartridge tab, symbols in every format, the trace,
# Save, the prompt, and detach resumes.
studio2_session_test(debug_test debug_test.c)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up (and the debugger's
# FujiNet trace saw both ways), and comes up again after a power cycle. SKIPs
# (77) when the runtime was not built (-DWITH_FUJINET=OFF). Serial: it owns
# the port.
studio2_session_test(fujibus_smoke fujibus_smoke.c)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# CONFIG, driven like a user, boots the suite's own app from the SD host
# through FujiNet (screens recognised by their text in the cart's text
# engine). SKIPs (77) without the FujiNet runtime. Serial: it owns the port.
studio2_session_test(config_boot_test config_boot_test.c)
add_test(NAME config_boot COMMAND config_boot_test)
set_tests_properties(config_boot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)

# A network boot end to end: the cartridge bring-up's fujitest client makes a
# real transaction and fujiboot mounts S2COUNTER.BIN (put there by Import
# Cartridge to SD) from the SD host, FujiNet pushes it, the cart stages and
# swaps it in, and the stand-in runs it; twice, with the mailbox on its worker
# thread and inline. Needs STUDIO2_TESTROM_DIR (fujinet-firmware
# pico/studio2/build, built with BOOT_HOST=0 BOOT_PATH=/S2COUNTER.BIN); SKIPs
# without.
studio2_session_test(netboot_test netboot_test.c)
add_test(NAME netboot COMMAND netboot_test)
set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)
add_test(NAME netboot_sync COMMAND netboot_test)
set_tests_properties(netboot_sync PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON
  ENVIRONMENT "STUDIO2_CART_SYNC=1")

endif()
endfunction()

# The core links and runs alone: no SDL, no session (a session symbol would
# fail the link).
studio2_test(no_sdl_link_test no_sdl_link_test.c)
add_test(NAME no_sdl_link COMMAND no_sdl_link_test)

# A headless runner: boots a machine and writes frames as PPM (the MAME A/B
# harness's "side B", tools/ab/). Built, not registered.
studio2_test(studio2_headless studio2_headless.c)

# The disassembler against MAME's own, opcode by opcode (dasm_golden.h).
studio2_test(dasm_test dasm_test.c)
add_test(NAME dasm COMMAND dasm_test)

# The core held to MAME read for read, with no RCA code: the tests' own BIOS
# and game, and CONFIG on the stand-in, against folds of MAME's own traces
# (tools/ab/README.md).
studio2_test(mame_trace_test mame_trace_test.c)
add_test(NAME mame_trace COMMAND mame_trace_test)
set_tests_properties(mame_trace PROPERTIES TIMEOUT 120)

# The paced host: CONFIG paints, the machine keeps its own frame rate.
studio2_test(boot_smoke boot_smoke.c)
add_test(NAME boot_smoke COMMAND boot_smoke)
set_tests_properties(boot_smoke PROPERTIES TIMEOUT 60)

# The no-BIOS stand-in: CONFIG and a FujiNet app boot on it, a game parks it,
# CLEAR restarts, keys reach the program, a BIOS of the tests' own runs the
# game (test_rom.h, from roms/*.asm).
studio2_test(stub_test stub_test.c)
add_test(NAME stub_boot COMMAND stub_test)

# The cartridge device and its transport against a stand-in FujiNet on
# loopback: a MOUNT_IMAGE with a pushed image, the staging, BOOTLOCK and the
# swap, and the trace tap held to the bytes on the wire. POSIX only (77).
studio2_test(cart_test cart_test.c)
add_test(NAME cart COMMAND cart_test)
set_tests_properties(cart PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The debugger's FujiNet trace on byte streams built in the test: both
# directions, split frames, a send dropping a cut-short reply, repeats and
# repeated exchanges counted as one, the ring's limit and Clear.
studio2_test(fujitrace_test fujitrace_test.c)
add_test(NAME fujitrace COMMAND fujitrace_test)

# These need the session layer, which needs SDL3. On a build without it
# (a cross-toolchain, or a lean machine) only the core libraries build.
if(TARGET studio2_session)

function(studio2_session_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE studio2_session)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/debugger")
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE
    STUDIO2_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  # The debugger contract: attach stops the machine, registers, every step
# kind, disassembly, breakpoints of every class with conditions, memory, the
# video views, I/O, the cartridge tab, symbols in every format, the trace,
# Save, the prompt, and detach resumes.
studio2_session_test(debug_test debug_test.c)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up (and the debugger's
# FujiNet trace saw both ways), and comes up again after a power cycle. SKIPs
# (77) when the runtime was not built (-DWITH_FUJINET=OFF). Serial: it owns
# the port.
studio2_session_test(fujibus_smoke fujibus_smoke.c)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# CONFIG, driven like a user, boots the suite's own app from the SD host
# through FujiNet (screens recognised by their text in the cart's text
# engine). SKIPs (77) without the FujiNet runtime. Serial: it owns the port.
studio2_session_test(config_boot_test config_boot_test.c)
add_test(NAME config_boot COMMAND config_boot_test)
set_tests_properties(config_boot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)

# A network boot end to end: the cartridge bring-up's fujitest client makes a
# real transaction and fujiboot mounts S2COUNTER.BIN (put there by Import
# Cartridge to SD) from the SD host, FujiNet pushes it, the cart stages and
# swaps it in, and the stand-in runs it; twice, with the mailbox on its worker
# thread and inline. Needs STUDIO2_TESTROM_DIR (fujinet-firmware
# pico/studio2/build, built with BOOT_HOST=0 BOOT_PATH=/S2COUNTER.BIN); SKIPs
# without.
studio2_session_test(netboot_test netboot_test.c)
add_test(NAME netboot COMMAND netboot_test)
set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)
add_test(NAME netboot_sync COMMAND netboot_test)
set_tests_properties(netboot_sync PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON
  ENVIRONMENT "STUDIO2_CART_SYNC=1")

endif()
endfunction()

# The frontend contract, against a real temporary XDG tree: settings persist,
# paths resolve, CONFIG boots and paints on the stand-in, input reaches the
# machine, a FujiNet app opens, CLEAR restarts it, Reset to CONFIG ejects it,
# a game is refused until a BIOS is imported, an image the cartridge cannot
# plan is refused with a reason.
studio2_session_test(session_test session_test.c)
add_test(NAME session COMMAND session_test)
set_tests_properties(session PROPERTIES TIMEOUT 120)

# The remappable binding table and the keyboard translator, pure.
studio2_session_test(bindings_test bindings_test.c)
add_test(NAME bindings COMMAND bindings_test)

# Where a dropped file goes, Import Cartridge to SD, and Import BIOS
# (identified by size and CRC; no RCA firmware involved).
studio2_session_test(media_test media_test.c)
add_test(NAME media COMMAND media_test)

# The gamepad layer's pure helpers, and that the SDL subsystem starts and
# stops with no hardware attached.
studio2_session_test(gamepad_test gamepad_test.c)
add_test(NAME gamepad COMMAND gamepad_test)

# The gamepad layer end to end with an SDL3 virtual gamepad: hot-plug, the
# default mapping onto the keypad and CLEAR, unplug releasing what was held.
# SKIPs (77) where SDL has no virtual joysticks.
studio2_session_test(gamepad_map_test gamepad_map_test.c)
add_test(NAME gamepad_map COMMAND gamepad_map_test)
set_tests_properties(gamepad_map PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The debugger contract: attach stops the machine, registers, every step
# kind, disassembly, breakpoints of every class with conditions, memory, the
# video views, I/O, the cartridge tab, symbols in every format, the trace,
# Save, the prompt, and detach resumes.
studio2_session_test(debug_test debug_test.c)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up (and the debugger's
# FujiNet trace saw both ways), and comes up again after a power cycle. SKIPs
# (77) when the runtime was not built (-DWITH_FUJINET=OFF). Serial: it owns
# the port.
studio2_session_test(fujibus_smoke fujibus_smoke.c)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# CONFIG, driven like a user, boots the suite's own app from the SD host
# through FujiNet (screens recognised by their text in the cart's text
# engine). SKIPs (77) without the FujiNet runtime. Serial: it owns the port.
studio2_session_test(config_boot_test config_boot_test.c)
add_test(NAME config_boot COMMAND config_boot_test)
set_tests_properties(config_boot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)

# A network boot end to end: the cartridge bring-up's fujitest client makes a
# real transaction and fujiboot mounts S2COUNTER.BIN (put there by Import
# Cartridge to SD) from the SD host, FujiNet pushes it, the cart stages and
# swaps it in, and the stand-in runs it; twice, with the mailbox on its worker
# thread and inline. Needs STUDIO2_TESTROM_DIR (fujinet-firmware
# pico/studio2/build, built with BOOT_HOST=0 BOOT_PATH=/S2COUNTER.BIN); SKIPs
# without.
studio2_session_test(netboot_test netboot_test.c)
add_test(NAME netboot COMMAND netboot_test)
set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)
add_test(NAME netboot_sync COMMAND netboot_test)
set_tests_properties(netboot_sync PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON
  ENVIRONMENT "STUDIO2_CART_SYNC=1")

endif()
