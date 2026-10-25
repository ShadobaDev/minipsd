cmake_minimum_required(VERSION 3.21)   # PROJECT_IS_TOP_LEVEL

# ---------------------------------------------------------------------------
# Version: read from minipsd.h, which a raw-copy user always has, so the header, the CMake
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
    DESCRIPTION  "Header-only, dependency-free C99 writer and reader of layered Photoshop (PSD) files"
    HOMEPAGE_URL "https://github.com/ShadobaDev/minipsd"
    LANGUAGES    NONE
)

# Tests and install rules are for working on minipsd itself, not for consumers.
option(MINIPSD_BUILD_TESTS "Build the minipsd tests"          ${PROJECT_IS_TOP_LEVEL})
option(MINIPSD_INSTALL     "Generate the minipsd install rules" ${PROJECT_IS_TOP_LEVEL})
# The libvips plugin (libvips-plugin/): psdload and psdsave for a program that links libvips.
option(MINIPSD_VIPS_PLUGIN "Build the libvips plugin, minipsd::vips (libvips fetched on Windows)" OFF)

if(PROJECT_IS_TOP_LEVEL OR MINIPSD_BUILD_TESTS OR MINIPSD_VIPS_PLUGIN)
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
file(GLOB_RECURSE _minipsd_headers RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
     CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
list(SORT _minipsd_headers)
# Absolute paths: relative ones would be taken from the source root, where no header is, and an
# edited header would refresh neither the checksums here nor tests/'s single-file header.
list(TRANSFORM _minipsd_headers PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/include/" OUTPUT_VARIABLE _minipsd_header_paths)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_minipsd_header_paths}")
include(cmake/minipsd_spdx.cmake)
minipsd_spdx(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/minipsd.spdx.json" PACKAGE minipsd VERSION "${PROJECT_VERSION}"
             HOMEPAGE "${PROJECT_HOMEPAGE_URL}" DOWNLOAD "git+${PROJECT_HOMEPAGE_URL}.git@${PROJECT_VERSION}"
             SUMMARY "${PROJECT_DESCRIPTION}" COMMENT "Header-only: the files below, under include/, are the whole library."
             BASE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "include/" FILES ${_minipsd_headers})
set_target_properties(minipsd PROPERTIES MINIPSD_SBOM_FILE "${CMAKE_CURRENT_BINARY_DIR}/minipsd.spdx.json")

# ---------------------------------------------------------------------------
# Install: headers, CMake package, pkg-config file, licence and SBOM. Everything is
# architecture-independent, so it goes under share/ rather than lib/.
# ---------------------------------------------------------------------------
if(MINIPSD_INSTALL)
    include(CMakePackageConfigHelpers)
    set(_minipsd_cmake_dir "${CMAKE_INSTALL_DATADIR}/cmake/minipsd")
    # Not CMAKE_INSTALL_DOCDIR: as a subproject it would name the parent project.
    set(_minipsd_doc_dir   "${CMAKE_INSTALL_DATAROOTDIR}/doc/minipsd")

    install(TARGETS minipsd EXPORT minipsd-targets)
    install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
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

    # Packages of the install tree: minipsd-X.Y.Z.tar.gz / .zip (any platform; unpack and point
    # CMAKE_PREFIX_PATH at it, or extract into a prefix) and, with -G DEB, libminipsd-dev.
    # Nothing in the tree depends on the platform, so neither name carries one.
    set(CPACK_PACKAGE_NAME                minipsd)
    set(CPACK_PACKAGE_VENDOR              "Bartlomiej Mucha")
    set(CPACK_PACKAGE_CONTACT             "Bartlomiej Mucha <shadobadev@gmail.com>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_RESOURCE_FILE_LICENSE       "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.MIT")
    set(CPACK_PACKAGE_FILE_NAME           "minipsd-${PROJECT_VERSION}")
    set(CPACK_OUTPUT_FILE_PREFIX          "${CMAKE_CURRENT_SOURCE_DIR}/dist")
    set(CPACK_PACKAGE_DIRECTORY           "${CMAKE_CURRENT_BINARY_DIR}")   # staging, not the cwd
    set(CPACK_GENERATOR                   "TGZ;ZIP")
    set(CPACK_DEBIAN_PACKAGE_NAME         libminipsd-dev)
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE all)
    set(CPACK_DEBIAN_PACKAGE_SECTION      libdevel)
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE     "${PROJECT_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_FILE_NAME            DEB-DEFAULT)   # libminipsd-dev_X.Y.Z_all.deb
    include(CPack)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MINIPSD_BUILD_TESTS)
    enable_language(CXX)
    enable_testing()
    add_subdirectory(tests)   # the plugin too, with the tests' warnings and its own test
elseif(MINIPSD_VIPS_PLUGIN)
    include(cmake/minipsd_libvips.cmake)
    add_subdirectory(libvips-plugin)
endif()
