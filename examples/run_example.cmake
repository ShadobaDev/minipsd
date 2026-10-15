# Runs one example in an empty directory and checks the one .psd it leaves there.
#   cmake -DEXE=<example> [-DARG=<argument>] -DDIR=<directory> -DSHA256=<hash> [-DNAME_HEX=<hex>]
#         -P run_example.cmake
#   SHA256    the file's hash: the golden test's, tests/test_golden.cpp
#   NAME_HEX  the file's name, as the bytes in hex (string(HEX)), for a name outside ASCII
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
execute_process(COMMAND "${EXE}" ${ARG} WORKING_DIRECTORY "${DIR}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${EXE} failed: ${rc}")
endif()

file(GLOB files RELATIVE "${DIR}" "${DIR}/*.psd")
list(LENGTH files count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "expected one .psd in ${DIR}, found: ${files}")
endif()
if(DEFINED NAME_HEX)
    string(HEX "${files}" hex)
    if(NOT hex STREQUAL NAME_HEX)
        message(FATAL_ERROR "the file is named ${files} (${hex}), expected the bytes ${NAME_HEX}")
    endif()
endif()
file(SHA256 "${DIR}/${files}" hash)
if(NOT hash STREQUAL SHA256)
    message(FATAL_ERROR "${files}: SHA-256 ${hash}, expected ${SHA256}")
endif()
