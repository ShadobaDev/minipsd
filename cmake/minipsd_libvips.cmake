# ---------------------------------------------------------------------------
# libvips for what uses it here: examples/vips_composite.c (MINIPSD_VIPS_EXAMPLE) and the plugin
# in libvips-plugin/ (MINIPSD_VIPS_PLUGIN).  minipsd itself never depends on libvips.
#
# Windows fetches libvips' official prebuilt "web" build, pinned (the newest release when set);
# elsewhere pkg-config finds the installed one.  Gives:
#
#   minipsd_libvips        an imported target: libvips' C API, glib and gobject
#   MINIPSD_LIBVIPS_ENV    the environment its tests run in (Windows: the DLLs on PATH), or ""
# ---------------------------------------------------------------------------

if(WIN32)
    set(MINIPSD_VIPS_VERSION 8.18.7)
    include(FetchContent)
    FetchContent_Declare(minipsd_vips
        URL https://github.com/libvips/build-win64-mxe/releases/download/v${MINIPSD_VIPS_VERSION}/vips-dev-x64-web-${MINIPSD_VIPS_VERSION}.zip
        URL_HASH SHA256=3a122eb3d588690008216f786338e2f9329dba3c3c2768b5933cf7299d549241
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake-project)   # files only: nothing to add as a subproject
    FetchContent_MakeAvailable(minipsd_vips)
    set(_vips "${minipsd_vips_SOURCE_DIR}")
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
    set(_vips_path "${_vips}/bin;$ENV{PATH}")   # the DLLs, for the tests
    string(REPLACE ";" "\\;" _vips_path "${_vips_path}")
    set(MINIPSD_LIBVIPS_ENV "PATH=${_vips_path}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips)
    add_library(minipsd_libvips INTERFACE IMPORTED)
    target_link_libraries(minipsd_libvips INTERFACE PkgConfig::VIPS)
    set(MINIPSD_LIBVIPS_ENV "")
endif()
