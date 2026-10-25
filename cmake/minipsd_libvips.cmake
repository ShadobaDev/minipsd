# ---------------------------------------------------------------------------
# libvips for what uses it: minipsd's libvips plugin (libvips-plugin/, MINIPSD_VIPS_PLUGIN; or the
# plugin as released, minipsd-vips-<version>.zip) and examples/vips_composite.c
# (MINIPSD_VIPS_EXAMPLE).  minipsd itself never depends on libvips.
#
# Windows fetches libvips' official prebuilt "web" build, pinned (the newest release when set);
# elsewhere pkg-config finds the installed one.  Gives:
#
#   minipsd_libvips        an imported target: libvips' C API, glib and gobject
#   minipsd_libvips_zlib   the zlib libvips links: Windows, the zip's libz1 (zlib-ng, built
#                          zlib-compatible; versions.json) with zlib's own headers, fetched and
#                          pinned, since the zip has none; elsewhere pkg-config's zlib
#   MINIPSD_LIBVIPS_ENV    the environment its tests run in (Windows: the DLLs on PATH), or ""
#
# A program with libvips of its own defines both targets before adding minipsd, visible to it (an
# INTERFACE library, or IMPORTED GLOBAL); nothing is then fetched or found here.  A program that
# fetched libvips' Windows build itself sets FETCHCONTENT_SOURCE_DIR_MINIPSD_LIBVIPS to where it is
# unpacked: that one is used, and only zlib's headers are fetched.
# ---------------------------------------------------------------------------

if(TARGET minipsd_libvips AND TARGET minipsd_libvips_zlib)
    set(MINIPSD_LIBVIPS_ENV "")
    return()
endif()

if(WIN32)
    set(MINIPSD_VIPS_VERSION 8.18.7)
    include(FetchContent)
    FetchContent_Declare(minipsd_libvips   # named as its target: a program may call the plugin minipsd_vips
        URL https://github.com/libvips/build-win64-mxe/releases/download/v${MINIPSD_VIPS_VERSION}/vips-dev-x64-web-${MINIPSD_VIPS_VERSION}.zip
        URL_HASH SHA256=3a122eb3d588690008216f786338e2f9329dba3c3c2768b5933cf7299d549241
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake-project)   # files only: nothing to add as a subproject
    FetchContent_MakeAvailable(minipsd_libvips)
    set(_vips "${minipsd_libvips_SOURCE_DIR}")
    add_library(minipsd_libvips INTERFACE IMPORTED)
    if(MSVC)   # the zip's import libraries
        target_link_libraries(minipsd_libvips INTERFACE
            "${_vips}/lib/libvips.lib" "${_vips}/lib/libglib-2.0.lib" "${_vips}/lib/libgobject-2.0.lib")
    else()     # MinGW links the DLLs themselves
        target_link_libraries(minipsd_libvips INTERFACE
            "${_vips}/bin/libvips-42.dll" "${_vips}/bin/libglib-2.0-0.dll" "${_vips}/bin/libgobject-2.0-0.dll")
    endif()
    target_include_directories(minipsd_libvips SYSTEM INTERFACE
        "${_vips}/include" "${_vips}/include/glib-2.0" "${_vips}/lib/glib-2.0/include")
    FetchContent_Declare(minipsd_zlib_headers
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake-project)   # zlib.h and zconf.h only
    FetchContent_MakeAvailable(minipsd_zlib_headers)
    add_library(minipsd_libvips_zlib INTERFACE IMPORTED)
    target_include_directories(minipsd_libvips_zlib SYSTEM INTERFACE "${minipsd_zlib_headers_SOURCE_DIR}")
    if(MSVC)
        target_link_libraries(minipsd_libvips_zlib INTERFACE "${_vips}/lib/libz1.lib")
    else()
        target_link_libraries(minipsd_libvips_zlib INTERFACE "${_vips}/bin/libz1.dll")
    endif()
    set(_vips_path "${_vips}/bin;$ENV{PATH}")   # the DLLs, for the tests
    string(REPLACE ";" "\\;" _vips_path "${_vips_path}")
    set(MINIPSD_LIBVIPS_ENV "PATH=${_vips_path}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips)
    add_library(minipsd_libvips INTERFACE IMPORTED)
    target_link_libraries(minipsd_libvips INTERFACE PkgConfig::VIPS)
    pkg_check_modules(VIPS_ZLIB REQUIRED IMPORTED_TARGET zlib)
    add_library(minipsd_libvips_zlib INTERFACE IMPORTED)
    target_link_libraries(minipsd_libvips_zlib INTERFACE PkgConfig::VIPS_ZLIB)
    set(MINIPSD_LIBVIPS_ENV "")
endif()
