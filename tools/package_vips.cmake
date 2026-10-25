# Packs the libvips plugin as released: <OUTPUT_DIR>/minipsd-vips-<version>.zip, a CMake project
# that builds as it is (libvips-plugin/release/README.md).  Its tree:
#
#   CMakeLists.txt, README.md, example.c   libvips-plugin/release/
#   minipsd_vips.h, minipsd_vips.c         the plugin's own: its API, its registration
#   foreign/                               libvips-plugin/foreign/: what libvips/foreign/ would hold
#   foreign/minipsd/minipsd.h              include/ joined by tools/amalgamate.cmake, carried as
#                                          libvips carries libnsgif and qoi
#   cmake/minipsd_libvips.cmake            as the repository finds libvips
#   LICENSE.MIT
#
#   cmake -DOUTPUT_DIR=<dir> -P tools/package_vips.cmake
cmake_minimum_required(VERSION 3.21)

if(NOT OUTPUT_DIR)
    message(FATAL_ERROR "usage: cmake -DOUTPUT_DIR=<dir> -P tools/package_vips.cmake")
endif()
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(_out "${OUTPUT_DIR}" ABSOLUTE)

# The version, as the project reads it: from minipsd.h.
file(STRINGS "${_root}/include/minipsd.h" _lines REGEX "^#define MINIPSD_VERSION_(MAJOR|MINOR|PATCH) +[0-9]+$")
set(_version "")
foreach(_part MAJOR MINOR PATCH)
    if(NOT _lines MATCHES "MINIPSD_VERSION_${_part} +([0-9]+)")
        message(FATAL_ERROR "minipsd.h does not define MINIPSD_VERSION_${_part}")
    endif()
    list(APPEND _version "${CMAKE_MATCH_1}")
endforeach()
list(JOIN _version "." _version)

set(_name "minipsd-vips-${_version}")
set(_stage "${_out}/${_name}")
file(REMOVE_RECURSE "${_stage}")
file(REMOVE "${_out}/${_name}.zip")

file(GLOB _foreign "${_root}/libvips-plugin/foreign/*.c" "${_root}/libvips-plugin/foreign/*.h")
file(COPY ${_foreign} DESTINATION "${_stage}/foreign")
file(COPY "${_root}/libvips-plugin/minipsd_vips.c" "${_root}/libvips-plugin/minipsd_vips.h" DESTINATION "${_stage}")
file(COPY "${_root}/libvips-plugin/release/CMakeLists.txt" "${_root}/libvips-plugin/release/README.md"
          "${_root}/libvips-plugin/release/example.c" "${_root}/LICENSE.MIT" DESTINATION "${_stage}")
file(COPY "${_root}/cmake/minipsd_libvips.cmake" DESTINATION "${_stage}/cmake")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${_stage}/foreign/minipsd/minipsd.h" -P "${_root}/tools/amalgamate.cmake"
                RESULT_VARIABLE _rc)
if(_rc)
    message(FATAL_ERROR "tools/amalgamate.cmake failed")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${_name}.zip" --format=zip "${_name}"
                WORKING_DIRECTORY "${_out}" RESULT_VARIABLE _rc)
if(_rc)
    message(FATAL_ERROR "zipping ${_stage} failed")
endif()
message(STATUS "${_out}/${_name}.zip")
