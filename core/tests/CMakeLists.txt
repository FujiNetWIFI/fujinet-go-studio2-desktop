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
