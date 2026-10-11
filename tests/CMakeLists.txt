# ---------------------------------------------------------------------------
# minipsd tests
#
#   minipsd-tests      GoogleTest: the writer, read back field by field, built as C99 + C++20
#   minipsd-cxx17      compile-only: the header and its implementation as C++17
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

add_executable(minipsd-tests
    test_minipsd.cpp
    minipsd_c_smoke.c   # minipsd.h compiled as C, compared byte for byte with the C++ build
)
target_link_libraries(minipsd-tests PRIVATE minipsd::minipsd GTest::gtest GTest::gtest_main)
target_compile_options(minipsd-tests PRIVATE ${MINIPSD_WARNINGS})
set_target_properties(minipsd-tests PROPERTIES
    C_STANDARD   99 C_STANDARD_REQUIRED   ON C_EXTENSIONS   OFF
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF   # the tests use std::u8string
)
if(MINGW)
    # Self-contained executable: runs without the MinGW runtime DLLs on PATH.
    target_link_options(minipsd-tests PRIVATE -static)
endif()
gtest_discover_tests(minipsd-tests)

# The contract promises C++17; the tests themselves need C++20, so check 17 separately.
add_library(minipsd-cxx17 OBJECT cxx17_check.cpp)
target_link_libraries(minipsd-cxx17 PRIVATE minipsd::minipsd)
target_compile_options(minipsd-cxx17 PRIVATE ${MINIPSD_WARNINGS})
set_target_properties(minipsd-cxx17 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

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
