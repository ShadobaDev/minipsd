# ---------------------------------------------------------------------------
# minipsd examples: the I/O minipsd leaves to its caller, each a complete program on the public
# API. Built and run as tests (with MINIPSD_BUILD_TESTS), never installed. Each writer but the
# hello pair writes the golden test's "template-rle" document (demo.h), and its test checks the
# bytes by SHA-256.
#
#   hello_write        the shortest writer: one layer to hello.psd     (the manual's Getting started)
#   hello_read         the shortest reader: layer names and the composite of hello.psd
#   write_file         minipsd_write to stdio, a UTF-8 path on Windows included   (blocking)
#   write_memory       the pull writer into one buffer of the exact size
#   write_nonblocking  the pull writer into an O_NONBLOCK pipe, with poll          (POSIX only)
#   read_file          a PSD read whole, described, every layer and the composite decoded
#   resave             a PSD read as a document, one layer changed, written again   (round trip)
#   vips_composite     layers flattened by libvips, written with that composite  (MINIPSD_VIPS_EXAMPLE)
# ---------------------------------------------------------------------------

set(_examples hello_write hello_read write_file write_memory read_file resave)
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

# The hello pair, in one directory: hello_read reads what hello_write wrote.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/out-hello")
add_test(NAME example.hello_write COMMAND hello_write WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/out-hello")
add_test(NAME example.hello_read  COMMAND hello_read  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/out-hello")
set_tests_properties(example.hello_write PROPERTIES FIXTURES_SETUP minipsd-hello-psd)
set_tests_properties(example.hello_read PROPERTIES
    FIXTURES_REQUIRED minipsd-hello-psd
    PASS_REGULAR_EXPRESSION "64 x 64, 1 layers
layer 'Hello'
first pixel: 255 0 0 255")

# Failures are reported, not swallowed.
add_test(NAME example.write_file.missing-dir COMMAND write_file "${CMAKE_CURRENT_BINARY_DIR}/missing/x.psd")
set_tests_properties(example.write_file.missing-dir PROPERTIES PASS_REGULAR_EXPRESSION "cannot open")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # /dev/full opens, but every write to it fails.
    add_test(NAME example.write_file.dev-full COMMAND write_file /dev/full)
    set_tests_properties(example.write_file.dev-full PROPERTIES PASS_REGULAR_EXPRESSION "write failed|cannot open or close")
endif()

# read_file: on what write_memory wrote, and on every PSD from other programs (tests/data,
# committed; tests/fixtures, local only, unless MINIPSD_TEST_FIXTURES is off).  A layer or
# composite that fails to decode fails it.
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
 +[0-9]+ visible pixels[^
]*
as a document: everything kept"
    FAIL_REGULAR_EXPRESSION "not decoded")
file(GLOB _other_psds "${PROJECT_SOURCE_DIR}/tests/data/*.psd")
if(_minipsd_fixtures)
    file(GLOB _local_psds "${_minipsd_fixtures}/*.psd")
    list(APPEND _other_psds ${_local_psds})
endif()
foreach(_psd IN LISTS _other_psds)
    get_filename_component(_name "${_psd}" NAME_WE)
    add_test(NAME example.read_file.${_name} COMMAND read_file "${_psd}")
    set_tests_properties(example.read_file.${_name} PROPERTIES FAIL_REGULAR_EXPRESSION "not decoded")
endforeach()

# resave: what write_memory wrote, its bottom layer inverted; read_file then reads the result and
# finds nothing lost.  On files from other programs, it must write something read_file decodes.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/out-resave")
add_test(NAME example.resave COMMAND resave "${CMAKE_CURRENT_BINARY_DIR}/out-write_memory/out.psd"
                                            "${CMAKE_CURRENT_BINARY_DIR}/out-resave/resaved.psd")
set_tests_properties(example.resave PROPERTIES
    FIXTURES_REQUIRED minipsd-example-psd FIXTURES_SETUP minipsd-resaved-psd
    PASS_REGULAR_EXPRESSION "layer 0 'Template' inverted; 0 other layers copied as stored")
add_test(NAME example.resave.read COMMAND read_file "${CMAKE_CURRENT_BINARY_DIR}/out-resave/resaved.psd")
set_tests_properties(example.resave.read PROPERTIES
    FIXTURES_REQUIRED minipsd-resaved-psd
    PASS_REGULAR_EXPRESSION "0 'Template'.*as a document: everything kept"
    FAIL_REGULAR_EXPRESSION "not decoded")
foreach(_psd IN LISTS _other_psds)
    get_filename_component(_name "${_psd}" NAME_WE)
    add_test(NAME example.resave.${_name}
        COMMAND resave "${_psd}" "${CMAKE_CURRENT_BINARY_DIR}/out-resave/${_name}.psd")
    set_tests_properties(example.resave.${_name} PROPERTIES PASS_REGULAR_EXPRESSION "inverted")
endforeach()

# vips_composite: libvips renders the composite, minipsd writes the file with it (MINIPSD_VIPS_EXAMPLE,
# off by default: it needs libvips, from cmake/minipsd_libvips.cmake).  minipsd never depends on
# libvips; only this example does.  It links the C API only.
if(MINIPSD_VIPS_EXAMPLE)
    add_executable(vips_composite vips_composite.c)
    target_link_libraries(vips_composite PRIVATE minipsd::minipsd minipsd_libvips)
    target_compile_options(vips_composite PRIVATE ${MINIPSD_WARNINGS})
    set_target_properties(vips_composite PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_definitions(vips_composite PRIVATE _CRT_SECURE_NO_WARNINGS)   # plain fopen
    endif()

    # The layers made by the example; then, locally, a Photoshop file's (tests/fixtures).  Each run
    # reads its file back and says whether the composite came back as rendered.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/out-vips")
    add_test(NAME example.vips_composite COMMAND vips_composite "${CMAKE_CURRENT_BINARY_DIR}/out-vips/made.psd")
    set(_vips_tests example.vips_composite)
    if(_minipsd_fixtures AND EXISTS "${_minipsd_fixtures}/Test-g-example.psd")
        add_test(NAME example.vips_composite.Test-g-example
            COMMAND vips_composite "${CMAKE_CURRENT_BINARY_DIR}/out-vips/Test-g-example.psd" "${_minipsd_fixtures}/Test-g-example.psd")
        list(APPEND _vips_tests example.vips_composite.Test-g-example)
    endif()
    # TIMEOUT: libvips' exit has once hung on Windows, after the result; a hang fails fast.
    set_tests_properties(${_vips_tests} PROPERTIES PASS_REGULAR_EXPRESSION "composite read back: as rendered" TIMEOUT 120)
    if(MINIPSD_LIBVIPS_ENV)
        set_tests_properties(${_vips_tests} PROPERTIES ENVIRONMENT "${MINIPSD_LIBVIPS_ENV}")
    endif()
endif()
