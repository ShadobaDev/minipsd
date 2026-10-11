cmake_minimum_required(VERSION 3.21)   # PROJECT_IS_TOP_LEVEL

# ---------------------------------------------------------------------------
# Version: read from the header, the only file a raw-copy user has, so the header, the CMake
# package, minipsd.pc and the SBOM fragment can never disagree.
# ---------------------------------------------------------------------------
set(_minipsd_header "${CMAKE_CURRENT_LIST_DIR}/include/minipsd.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_minipsd_header}")
file(STRINGS "${_minipsd_header}" _minipsd_version_lines
     REGEX "^#define MINIPSD_VERSION_(MAJOR|MINOR|PATCH) +[0-9]+$")
set(_minipsd_version "")
foreach(_part MAJOR MINOR PATCH)
    if(NOT _minipsd_version_lines MATCHES "MINIPSD_VERSION_${_part} +([0-9]+)")
        message(FATAL_ERROR "minipsd.h does not define MINIPSD_VERSION_${_part}")
    endif()
    list(APPEND _minipsd_version "${CMAKE_MATCH_1}")
endforeach()
list(JOIN _minipsd_version "." _minipsd_version)

# LANGUAGES NONE: a header-only library costs a consumer nothing, not even a compiler check.
project(minipsd
    VERSION      ${_minipsd_version}
    DESCRIPTION  "Single-header, dependency-free C99 writer of layered Photoshop (PSD) files"
    HOMEPAGE_URL "https://github.com/ShadobaDev/minipsd"
    LANGUAGES    NONE
)

# Tests and install rules are for working on minipsd itself, not for consumers.
option(MINIPSD_BUILD_TESTS "Build the minipsd tests"          ${PROJECT_IS_TOP_LEVEL})
option(MINIPSD_INSTALL     "Generate the minipsd install rules" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL OR MINIPSD_BUILD_TESTS)
    enable_language(C)   # also gives GNUInstallDirs a known architecture
endif()
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
add_library(minipsd INTERFACE)
add_library(minipsd::minipsd ALIAS minipsd)
target_include_directories(minipsd INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# ---------------------------------------------------------------------------
# SBOM fragment: an SPDX 2.3 document describing minipsd, for a consumer to merge into its own
# SBOM. Found through the target property MINIPSD_SBOM_FILE, which works the same for
# add_subdirectory, FetchContent and find_package (a variable would not survive
# FetchContent_MakeAvailable's function scope). Its timestamp honours SOURCE_DATE_EPOCH.
# ---------------------------------------------------------------------------
file(SHA256 "${_minipsd_header}" MINIPSD_HEADER_SHA256)
string(TIMESTAMP MINIPSD_SBOM_CREATED "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(cmake/minipsd.spdx.json.in "${CMAKE_CURRENT_BINARY_DIR}/minipsd.spdx.json" @ONLY)
set_target_properties(minipsd PROPERTIES MINIPSD_SBOM_FILE "${CMAKE_CURRENT_BINARY_DIR}/minipsd.spdx.json")

# ---------------------------------------------------------------------------
# Install: header, CMake package, pkg-config file, licence and SBOM. Everything is
# architecture-independent, so it goes under share/ rather than lib/.
# ---------------------------------------------------------------------------
if(MINIPSD_INSTALL)
    include(CMakePackageConfigHelpers)
    set(_minipsd_cmake_dir "${CMAKE_INSTALL_DATADIR}/cmake/minipsd")
    # Not CMAKE_INSTALL_DOCDIR: as a subproject it would name the parent project.
    set(_minipsd_doc_dir   "${CMAKE_INSTALL_DATAROOTDIR}/doc/minipsd")

    install(TARGETS minipsd EXPORT minipsd-targets)
    install(FILES "${_minipsd_header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT minipsd-targets
        NAMESPACE   minipsd::
        DESTINATION "${_minipsd_cmake_dir}"
    )

    set(MINIPSD_INSTALL_SBOM "${_minipsd_doc_dir}/minipsd.spdx.json")
    configure_package_config_file(cmake/minipsd-config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/minipsd-config.cmake"
        INSTALL_DESTINATION "${_minipsd_cmake_dir}"
        PATH_VARS MINIPSD_INSTALL_SBOM
    )
    # Pre-1.0 a breaking change bumps the MINOR, so only the same MINOR is compatible.
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/minipsd-config-version.cmake"
        COMPATIBILITY SameMinorVersion
        ARCH_INDEPENDENT
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/minipsd-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/minipsd-config-version.cmake"
        DESTINATION "${_minipsd_cmake_dir}"
    )

    # The .pc file finds the prefix relative to itself, so the install tree is relocatable.
    string(REGEX REPLACE "[^/]+" ".." MINIPSD_PC_TO_PREFIX "${CMAKE_INSTALL_DATADIR}/pkgconfig")   # share/pkgconfig -> ../..
    configure_file(cmake/minipsd.pc.in "${CMAKE_CURRENT_BINARY_DIR}/minipsd.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/minipsd.pc" DESTINATION "${CMAKE_INSTALL_DATADIR}/pkgconfig")

    install(FILES LICENSE.MIT "${CMAKE_CURRENT_BINARY_DIR}/minipsd.spdx.json"
        DESTINATION "${_minipsd_doc_dir}"
    )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MINIPSD_BUILD_TESTS)
    enable_language(CXX)
    enable_testing()
    add_subdirectory(tests)
endif()
