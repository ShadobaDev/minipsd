# ---------------------------------------------------------------------------
# The libvips plugin: minipsd as libvips' psdload (MINIPSD_VIPS_PLUGIN).  A static library the
# program links, then registers with minipsd_vips_init() after VIPS_INIT(); minipsd is compiled
# into it, private (MINIPSD_STATIC).  libvips comes from cmake/minipsd_libvips.cmake.
#
#   minipsd::vips        the plugin
#   vips.plugin          documents minipsd writes and the PSDs at hand, loaded through libvips and
#                        compared with minipsd's own decode (test_vips.c)
# ---------------------------------------------------------------------------

add_library(minipsd_vips STATIC minipsd_vips.c minipsd_vips.h)
add_library(minipsd::vips ALIAS minipsd_vips)
target_include_directories(minipsd_vips PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(minipsd_vips PUBLIC minipsd_libvips PRIVATE minipsd::minipsd)
target_compile_options(minipsd_vips PRIVATE ${MINIPSD_WARNINGS})
set_target_properties(minipsd_vips PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

add_executable(minipsd-vips-test test_vips.c)
target_link_libraries(minipsd-vips-test PRIVATE minipsd::vips minipsd::minipsd)
target_compile_options(minipsd-vips-test PRIVATE ${MINIPSD_WARNINGS})
set_target_properties(minipsd-vips-test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_definitions(minipsd-vips-test PRIVATE _CRT_SECURE_NO_WARNINGS)   # plain fopen
endif()

# The files compared beside the documents: tests/data, then, when there, tests/fixtures and
# psd-tools' set (MINIPSD_PSD_TOOLS_FILES), one path a line.
file(GLOB _vips_files "${PROJECT_SOURCE_DIR}/tests/data/*.psd")
foreach(_dir IN ITEMS "${_minipsd_fixtures}" "${_minipsd_psd_tools_files}")
    if(_dir)
        file(GLOB_RECURSE _found "${_dir}/*.psd" "${_dir}/*.psb")
        list(APPEND _vips_files ${_found})
    endif()
endforeach()
list(JOIN _vips_files "\n" _vips_list)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/files.txt" "${_vips_list}\n")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/out")
add_test(NAME vips.plugin COMMAND minipsd-vips-test "${CMAKE_CURRENT_BINARY_DIR}/out" "${CMAKE_CURRENT_BINARY_DIR}/files.txt")
# TIMEOUT: libvips' exit has once hung on Windows (examples/), so a hang fails fast.
set_tests_properties(vips.plugin PROPERTIES PASS_REGULAR_EXPRESSION "vips plugin: all passed" TIMEOUT 120)
if(MINIPSD_LIBVIPS_ENV)
    set_tests_properties(vips.plugin PROPERTIES ENVIRONMENT "${MINIPSD_LIBVIPS_ENV}")
endif()
