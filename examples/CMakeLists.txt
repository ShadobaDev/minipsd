# ---------------------------------------------------------------------------
# minipsd examples: the I/O minipsd leaves to its caller, each a complete program on the public
# API. Built and run as tests (with MINIPSD_BUILD_TESTS), never installed. Each writes the golden
# test's "template-rle" document (demo.h), and its test checks the bytes by SHA-256.
#
#   write_file         minipsd_write to stdio, a UTF-8 path on Windows included   (blocking)
#   write_memory       the pull writer into one buffer of the exact size
#   write_nonblocking  the pull writer into an O_NONBLOCK pipe, with poll          (POSIX only)
#   read_file          a PSD read whole, described, every layer and the composite decoded
# ---------------------------------------------------------------------------

set(_examples write_file write_memory read_file)
if(UNIX)
    list(APPEND _examples write_nonblocking)
endif()

# "template-rle" in tests/test_golden.cpp
set(_golden 02584650a51891a7c7d56c455724f882534205116c8e1c0ca2d1cced7e2f3d93)

foreach(_example IN LISTS _examples)
    add_executable(${_example} ${_example}.c)
    target_link_libraries(${_example} PRIVATE minipsd::minipsd)
    target_compile_options(${_example} PRIVATE ${MINIPSD_WARNINGS})
    set_target_properties(${_example} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_definitions(${_example} PRIVATE _CRT_SECURE_NO_WARNINGS)   # plain fopen
    endif()
    if(MINGW)
        target_link_options(${_example} PRIVATE -static)
    endif()
endforeach()

function(minipsd_add_example_test name exe)
    add_test(NAME example.${name}
        COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:${exe}>" "-DDIR=${CMAKE_CURRENT_BINARY_DIR}/out-${name}"
                -DSHA256=${_golden} ${ARGN} -P "${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake")
endfunction()

# No argument: the example's own file name, "template-" a-ogonek ".psd" in UTF-8.
minipsd_add_example_test(write_file write_file -DNAME_HEX=74656d706c6174652dc4852e707364)
minipsd_add_example_test(write_memory write_memory -DARG=out.psd)
if(UNIX)
    minipsd_add_example_test(write_nonblocking write_nonblocking -DARG=out.psd)
endif()

# Failures are reported, not swallowed.
add_test(NAME example.write_file.missing-dir COMMAND write_file "${CMAKE_CURRENT_BINARY_DIR}/missing/x.psd")
set_tests_properties(example.write_file.missing-dir PROPERTIES PASS_REGULAR_EXPRESSION "cannot open")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # /dev/full opens, but every write to it fails.
    add_test(NAME example.write_file.dev-full COMMAND write_file /dev/full)
    set_tests_properties(example.write_file.dev-full PROPERTIES PASS_REGULAR_EXPRESSION "write failed|cannot open or close")
endif()

# read_file: on what write_memory wrote, and on every PSD from other programs (tests/data,
# committed; tests/fixtures, local only).  A layer or composite that fails to decode fails it.
set_tests_properties(example.write_memory PROPERTIES FIXTURES_SETUP minipsd-example-psd)
add_test(NAME example.read_file COMMAND read_file "${CMAKE_CURRENT_BINARY_DIR}/out-write_memory/out.psd")
set_tests_properties(example.read_file PROPERTIES
    FIXTURES_REQUIRED minipsd-example-psd
    PASS_REGULAR_EXPRESSION "300 x 7, 8-bit RGB.*0 'Template'.*1 'Layer'[^
]*
[^
]*
 +empty
composite:
 +[0-9]+ visible pixels"
    FAIL_REGULAR_EXPRESSION "not decoded")
file(GLOB _other_psds "${PROJECT_SOURCE_DIR}/tests/data/*.psd" "${PROJECT_SOURCE_DIR}/tests/fixtures/*.psd")
foreach(_psd IN LISTS _other_psds)
    get_filename_component(_name "${_psd}" NAME_WE)
    add_test(NAME example.read_file.${_name} COMMAND read_file "${_psd}")
    set_tests_properties(example.read_file.${_name} PROPERTIES FAIL_REGULAR_EXPRESSION "not decoded")
endforeach()
