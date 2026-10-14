# Joins include/minipsd.h and every header it includes into one self-contained minipsd.h: a release
# artifact for users who prefer a single file (docs/CODE-ARCH.md, Decisions, 3).
#
#   cmake -DOUTPUT=<file> -P tools/amalgamate.cmake
#
# A line `#include "x.h"` that starts in column 0 is replaced by x.h's text, each file once, in the
# order the includes appear; system includes stay.  The headers' own guards are kept, so the result
# behaves exactly like the directory: include it plainly, or after MINIPSD_IMPLEMENTATION.
cmake_minimum_required(VERSION 3.21)

if(NOT OUTPUT)
    message(FATAL_ERROR "usage: cmake -DOUTPUT=<file> -P tools/amalgamate.cmake")
endif()
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(_mpsd_inline path out)
    get_property(_seen GLOBAL PROPERTY MPSD_AMALGAMATED)
    if(path IN_LIST _seen)
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY MPSD_AMALGAMATED "${path}")

    file(READ "${path}" _text)
    get_filename_component(_dir "${path}" DIRECTORY)
    set(_result "")
    while(TRUE)
        string(REGEX MATCH "\n#include \"([^\"]+)\"[^\n]*" _line "${_text}")
        if(NOT _line)
            break()
        endif()
        set(_name "${CMAKE_MATCH_1}")
        string(FIND "${_text}" "${_line}" _at)
        string(LENGTH "${_line}" _length)
        math(EXPR _at_end "${_at} + ${_length}")
        math(EXPR _keep "${_at} + 1")                       # up to and with the newline before it
        string(SUBSTRING "${_text}" 0 ${_keep} _before)
        string(SUBSTRING "${_text}" ${_at_end} -1 _text)
        get_filename_component(_child "${_dir}/${_name}" ABSOLUTE)
        if(NOT EXISTS "${_child}")
            message(FATAL_ERROR "${path}: cannot find included \"${_name}\"")
        endif()
        _mpsd_inline("${_child}" _inlined)
        string(APPEND _result "${_before}" "${_inlined}")
    endwhile()
    string(APPEND _result "${_text}")
    set(${out} "${_result}" PARENT_SCOPE)
endfunction()

file(STRINGS "${_root}/include/minipsd.h" _version REGEX "^#define MINIPSD_VERSION_(MAJOR|MINOR|PATCH) ")
string(REGEX REPLACE "[^;]*MAJOR +([0-9]+);[^;]*MINOR +([0-9]+);[^;]*PATCH +([0-9]+)" "\\1.\\2.\\3" _version "${_version}")

_mpsd_inline("${_root}/include/minipsd.h" _body)
string(PREPEND _body
    "/* minipsd ${_version}, all of include/ joined into one file by tools/amalgamate.cmake.\n"
    "   Generated: edit the files in include/ instead. */\n")
# Not file(WRITE): on Windows it writes CRLF, and the artifact must be the same bytes everywhere.
# The template is the variable alone, so the text itself is never scanned for @...@.
file(CONFIGURE OUTPUT "${OUTPUT}" CONTENT "@_body@" @ONLY NEWLINE_STYLE UNIX)
message(STATUS "amalgamated minipsd ${_version}: ${OUTPUT}")
