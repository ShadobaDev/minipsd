# ---------------------------------------------------------------------------
# minipsd's libvips plugin, as released: psdload and psdsave for a program that links libvips.
#
#   add_subdirectory(minipsd-vips-<version>)
#   target_link_libraries(my-app PRIVATE minipsd::vips)
#
# then minipsd_vips_init() after VIPS_INIT() (README.md).  libvips: fetched on Windows (libvips'
# official prebuilt build, pinned), found with pkg-config elsewhere; or the program's own, as the
# targets minipsd_libvips and minipsd_libvips_zlib, defined before this (cmake/minipsd_libvips.cmake).
# minipsd is compiled into the plugin from include/minipsd.h, private to it: a program's own
# minipsd, of any version, is left alone.
#
#   minipsd::vips          the plugin, a static library
#   minipsd-vips-example   example.c: an image saved and read back through the plugin (its test)
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)   # PROJECT_IS_TOP_LEVEL
project(minipsd_vips LANGUAGES C)

option(MINIPSD_VIPS_EXAMPLE "Build example.c, and its test" ${PROJECT_IS_TOP_LEVEL})

include(cmake/minipsd_libvips.cmake)

file(GLOB _minipsd_vips_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
add_library(minipsd_vips STATIC ${_minipsd_vips_sources})
add_library(minipsd::vips ALIAS minipsd_vips)
target_include_directories(minipsd_vips PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(minipsd_vips PUBLIC minipsd_libvips PRIVATE minipsd_libvips_zlib)
set_target_properties(minipsd_vips PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

if(MINIPSD_VIPS_EXAMPLE)
    add_executable(minipsd-vips-example example.c)
    target_link_libraries(minipsd-vips-example PRIVATE minipsd::vips)
    set_target_properties(minipsd-vips-example PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    enable_testing()
    add_test(NAME minipsd-vips-example COMMAND minipsd-vips-example "${CMAKE_CURRENT_BINARY_DIR}/example.psd")
    if(MINIPSD_LIBVIPS_ENV)   # Windows: libvips' DLLs on PATH
        set_tests_properties(minipsd-vips-example PROPERTIES ENVIRONMENT "${MINIPSD_LIBVIPS_ENV}")
    endif()
endif()
