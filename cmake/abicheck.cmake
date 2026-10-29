# SPDX-License-Identifier: MIT
#
# 0037: the ELF's undefined symbols against the ABI.
#
#   cmake -DREADELF=<readelf> -DELF=<app.elf> -DEXPORTS=<exports.txt>
#         -DSTAMP=<file> -P cmake/abicheck.cmake
#
# Every symbol the ELF needs must be in exports.txt, or the host has no
# entry for it and esp_elf_relocate() fails on the board. A name in
# exports.txt the ELF no longer needs is only reported: removing it is
# an ABI change, and launchers built against it still link.

cmake_policy(SET CMP0057 NEW)   # IN_LIST, in script mode

# readelf exits non-zero on these ("no .dynamic section in the dynamic
# segment": project_elf() strips it, and esp_elf does not want it), but
# the symbol table it prints is whole. A missing table is the failure.
execute_process(COMMAND "${READELF}" -s --wide "${ELF}"
                OUTPUT_VARIABLE _out ERROR_QUIET)
if(NOT _out MATCHES "Symbol table")
    message(FATAL_ERROR "abicheck: no symbol table from ${READELF} in ${ELF}")
endif()
string(REPLACE "\n" ";" _lines "${_out}")
set(_need "")
foreach(_l IN LISTS _lines)
    if(_l MATCHES "GLOBAL +DEFAULT +UND +([A-Za-z_][A-Za-z0-9_]*)$")
        list(APPEND _need "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _need)
list(SORT _need)

file(STRINGS "${EXPORTS}" _have REGEX "^[A-Za-z_][A-Za-z0-9_]*$")

set(_missing "")
foreach(_s IN LISTS _need)
    if(NOT _s IN_LIST _have)
        list(APPEND _missing "${_s}")
    endif()
endforeach()
set(_unused "")
foreach(_s IN LISTS _have)
    if(NOT _s IN_LIST _need)
        list(APPEND _unused "${_s}")
    endif()
endforeach()

if(_unused)
    string(REPLACE ";" " " _unused "${_unused}")
    message(STATUS "abicheck: in exports.txt, not used by the ELF: ${_unused}")
endif()
if(_missing)
    string(REPLACE ";" "\n  " _missing "${_missing}")
    message(FATAL_ERROR "abicheck: the ELF needs symbols exports.txt does not have. "
            "This changes the ABI; add them to components/lothesome_abi/exports.txt:\n  ${_missing}")
endif()
list(LENGTH _need _n)
message(STATUS "abicheck: ${_n} symbols, all exported")
file(WRITE "${STAMP}" "${_n}\n")
