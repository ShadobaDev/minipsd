# ---------------------------------------------------------------------------
# minipsd tests
#
#   minipsd-tests      GoogleTest, one test_<module>.cpp per module: the writer read back field
#                      by field, the internal modules tested directly; built as C99 + C++20;
#                      plus allocation failures and the golden bytes of 0.1.0
#   minipsd-tests-amalgamated  the same tests against the single-file minipsd.h
#   minipsd-cxx17      compile-only: the header and its implementation as C++17
#   minipsd-include-check  compile-only: minipsd.h alone, then its implementation, as C
#   consumer-<mode>    a tiny project using minipsd each supported way (see consumer/)
# ---------------------------------------------------------------------------

# GoogleTest: a system install if there is one, otherwise fetched.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    set(gtest_force_shared_crt ON  CACHE BOOL "" FORCE)   # MSVC: the default /MD runtime
    set(BUILD_GMOCK            OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(GTest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(GTest)
endif()
include(GoogleTest)

if(MSVC)
    set(MINIPSD_WARNINGS /W4 /WX /permissive-)
else()
    set(MINIPSD_WARNINGS -Wall -Wextra -Wpedantic -Werror)
endif()

# Every test file includes only minipsd.h, so each also runs against the amalgamated file.
set(_minipsd_test_sources
    test_base.cpp
    test_document.cpp
    test_bytes.cpp
    test_utf.cpp
    test_packbits.cpp
    test_composite.cpp
    test_writer.cpp
    minipsd_c_smoke.c        # minipsd.h compiled as C, compared byte for byte with the C++ build
    test_alloc_failure.cpp   # minipsd.h with an allocator that fails on demand
    test_golden.cpp          # the bytes of 0.1.0, by SHA-256, for a fixed set of documents
)
# Each test file has its own static copy of the library and uses only part of it; whether the
# library leaves a function unused is checked by minipsd-include-check below instead.
if(MSVC)
    set(_minipsd_test_only_warnings /wd4505)
else()
    set(_minipsd_test_only_warnings -Wno-unused-function)
endif()

function(minipsd_add_test_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE GTest::gtest GTest::gtest_main)
    target_compile_options(${target} PRIVATE ${MINIPSD_WARNINGS} ${_minipsd_test_only_warnings})
    set_target_properties(${target} PROPERTIES
        C_STANDARD   99 C_STANDARD_REQUIRED   ON C_EXTENSIONS   OFF
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF   # the tests use std::u8string
    )
    if(MINGW)
        # Self-contained executable: runs without the MinGW runtime DLLs on PATH.
        target_link_options(${target} PRIVATE -static)
    endif()
endfunction()

minipsd_add_test_executable(minipsd-tests ${_minipsd_test_sources})
target_link_libraries(minipsd-tests PRIVATE minipsd::minipsd)
gtest_discover_tests(minipsd-tests)

# The same tests against the single file the release attaches (tools/amalgamate.cmake), with only
# that file's directory on the include path. Generated at configure time; editing a header
# reconfigures (the top-level CMakeLists.txt depends on every header).
set(_minipsd_amalgamated "${CMAKE_CURRENT_BINARY_DIR}/amalgamated/minipsd.h")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${_minipsd_amalgamated}" -P "${PROJECT_SOURCE_DIR}/tools/amalgamate.cmake"
    OUTPUT_QUIET
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/amalgamate.cmake")
minipsd_add_test_executable(minipsd-tests-amalgamated ${_minipsd_test_sources})
target_include_directories(minipsd-tests-amalgamated PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/amalgamated")
gtest_discover_tests(minipsd-tests-amalgamated TEST_PREFIX "amalgamated.")

# The contract promises C++17; the tests themselves need C++20, so check 17 separately.
add_library(minipsd-cxx17 OBJECT cxx17_check.cpp)
target_link_libraries(minipsd-cxx17 PRIVATE minipsd::minipsd)
target_compile_options(minipsd-cxx17 PRIVATE ${MINIPSD_WARNINGS})
set_target_properties(minipsd-cxx17 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# minipsd.h compiles alone, and its implementation leaves no internal function unused.
add_library(minipsd-include-check OBJECT include_check.c)
target_link_libraries(minipsd-include-check PRIVATE minipsd::minipsd)
target_compile_options(minipsd-include-check PRIVATE ${MINIPSD_WARNINGS})
set_target_properties(minipsd-include-check PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

# clang-tidy (the linux-tidy preset) on our own targets only, not on a fetched GoogleTest.
# Checks and options come from .clang-tidy at the repository root.
set(MINIPSD_CLANG_TIDY "" CACHE STRING "clang-tidy command to run on the test sources; empty = off")
if(MINIPSD_CLANG_TIDY)
    set_target_properties(minipsd-tests minipsd-cxx17 PROPERTIES
        C_CLANG_TIDY "${MINIPSD_CLANG_TIDY}" CXX_CLANG_TIDY "${MINIPSD_CLANG_TIDY}")
endif()

# ---------------------------------------------------------------------------
# psd-tools round trip: Python writes documents through the real minipsd (a shared build, via ctypes)
# and reads them back with psd-tools, an independent reader. Skipped when psd-tools is missing,
# unless MINIPSD_REQUIRE_PSD_TOOLS is set (CI sets it).
# ---------------------------------------------------------------------------
option(MINIPSD_REQUIRE_PSD_TOOLS "Fail the configure when psd-tools is not installed" OFF)
find_package(Python3 COMPONENTS Interpreter QUIET)
set(_psd_tools_rc 1)
if(Python3_FOUND)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import psd_tools"
                    RESULT_VARIABLE _psd_tools_rc OUTPUT_QUIET ERROR_QUIET)
endif()
if(CMAKE_C_FLAGS MATCHES "-fsanitize=")
    # A sanitized library does not load into an uninstrumented Python (linux-asan).
    message(STATUS "Sanitizer build: skipping the psd-tools round trip")
elseif(_psd_tools_rc EQUAL 0)
    add_library(minipsd-shared SHARED psd_tools/minipsd_shared.c)
    target_link_libraries(minipsd-shared PRIVATE minipsd::minipsd)
    target_compile_options(minipsd-shared PRIVATE ${MINIPSD_WARNINGS})
    set_target_properties(minipsd-shared PROPERTIES
        C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        WINDOWS_EXPORT_ALL_SYMBOLS ON)   # MSVC; MinGW exports everything by default
    if(MINGW)
        target_link_options(minipsd-shared PRIVATE -static-libgcc)   # loadable by any Python
    endif()
    add_test(NAME psd-tools-roundtrip
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/psd_tools/roundtrip.py"
                "$<TARGET_FILE:minipsd-shared>" "${CMAKE_CURRENT_BINARY_DIR}/psd-tools")
elseif(MINIPSD_REQUIRE_PSD_TOOLS)
    message(FATAL_ERROR "psd-tools is required but not importable by '${Python3_EXECUTABLE}' "
                        "(pip install -r tests/requirements.txt)")
else()
    message(STATUS "psd-tools not found: skipping the round trip (pip install -r tests/requirements.txt)")
endif()

# ---------------------------------------------------------------------------
# Consumer tests: configure, build and run consumer/ against this source tree, once per way of
# bringing minipsd in. "package" and "pkgconfig" use an install of this build.
# ---------------------------------------------------------------------------
set(_consumer_args
    -DMINIPSD_SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DMINIPSD_EXPECTED_VERSION=${PROJECT_VERSION}
    -DCMAKE_PREFIX_PATH=${CMAKE_CURRENT_BINARY_DIR}/prefix
)
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    list(APPEND _consumer_args -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
endif()
set(_generator_args --build-generator "${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND _generator_args --build-generator-platform "${CMAKE_GENERATOR_PLATFORM}")
endif()

set(_modes copy subdirectory fetchcontent)
if(MINIPSD_INSTALL)
    list(APPEND _modes package)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        list(APPEND _modes pkgconfig)
        list(APPEND _consumer_args -DPKG_CONFIG_EXECUTABLE=${PKG_CONFIG_EXECUTABLE})
    endif()

    add_test(NAME install
        COMMAND ${CMAKE_COMMAND} --install "${PROJECT_BINARY_DIR}"
                --prefix "${CMAKE_CURRENT_BINARY_DIR}/prefix" --config "$<CONFIG>")
    set_tests_properties(install PROPERTIES FIXTURES_SETUP minipsd-installed)
endif()

foreach(_mode IN LISTS _modes)
    add_test(NAME consumer-${_mode}
        COMMAND ${CMAKE_CTEST_COMMAND} --build-and-test
                "${CMAKE_CURRENT_SOURCE_DIR}/consumer" "${CMAKE_CURRENT_BINARY_DIR}/consumer-${_mode}"
                ${_generator_args}
                --build-config "$<CONFIG>"
                --build-options -DMODE=${_mode} ${_consumer_args}
                --test-command minipsd-consumer)
    if(_mode MATCHES "^(package|pkgconfig)$")
        set_tests_properties(consumer-${_mode} PROPERTIES FIXTURES_REQUIRED minipsd-installed)
    endif()
endforeach()
