# SPDX-License-Identifier: MIT
#
# 0036: the libraries this build was made from, for the licences page
# under the gear -- written as a header, libversions.h, that main
# includes. unprofitable-mahjong's m5tab5/cmake/libversions.cmake
# (itself defeatist's), cut to this firmware: each entry is a module, its
# checkout (a version, or a git commit's first seven characters) and its
# licence.
#
# From the records the build itself uses, so the screen cannot drift
# from what was compiled:
#
#   dependencies.lock          every managed component and the version the
#                              component manager resolved (git ones by
#                              commit); its licence from the licence file
#                              in managed_components/, the copy compiled
#   gen_ark12.py               the Ark Pixel commit the font was cut from,
#                              in feckless_graphics_handler's tools/
#   git                        this project's own commit
#
# Nothing is vendored here and nothing is fetched at build time, so that
# is all of it.
#
# Include AFTER project(). Sets LIBVERSIONS_DIR to the directory holding
# libversions.h, which is rewritten only when its text changes.
#
# Also runs on its own, for a look at what it finds without ESP-IDF:
#   cmake -P cmake/libversions.cmake      (writes ./libversions/; test/render
#                                          uses it so, for the page)

set(_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(_root "${_root}" ABSOLUTE)
set(_lock "")
if(COMMAND idf_build_get_property)
    idf_build_get_property(_lock DEPENDENCIES_LOCK)
endif()
if(NOT _lock)
    set(_lock "${_root}/dependencies.lock")
endif()
set(_libs "")   # "module|checkout|licence", one each

# A file whose change should re-run this; not in script mode (cmake -P),
# which has no directory to hang it on.
macro(_lv_depends f)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
    endif()
endmacro()

# A 40-hex git commit as its first seven; anything else as written.
function(_lv_short out ver)
    string(LENGTH "${ver}" _len)
    if(_len EQUAL 40 AND ver MATCHES "^[0-9a-f]+$")
        string(SUBSTRING "${ver}" 0 7 ver)
    endif()
    set(${out} "${ver}" PARENT_SCOPE)
endfunction()

# A component's licence, from the licence file in its directory, else
# its idf_component.yml's license field.
function(_lv_license out dir)
    set(_lic "")
    file(GLOB _files LIST_DIRECTORIES false
         "${dir}/license.txt" "${dir}/LICENSE" "${dir}/LICENSE.*"
         "${dir}/LICENCE" "${dir}/LICENSE-*")
    foreach(_f IN LISTS _files)
        file(STRINGS "${_f}" _head LIMIT_COUNT 15)
        string(JOIN "\n" _head ${_head})
        if(_head MATCHES "(Valid|SPDX)-License-Identifier: *([A-Za-z0-9.+-]+)")
            set(_lic "${CMAKE_MATCH_2}")
        elseif(_head MATCHES "Espressif Modified MIT")
            set(_lic "Espressif MIT")
        elseif(_head MATCHES "Apache License" AND _head MATCHES "Version 2\\.0")
            set(_lic "Apache-2.0")
        elseif(_head MATCHES "MIT License")
            set(_lic "MIT")
        elseif(_head MATCHES "Open Font License")
            set(_lic "OFL-1.1")
        endif()
        if(_lic)
            break()
        endif()
    endforeach()
    if(NOT _lic AND EXISTS "${dir}/idf_component.yml")
        file(STRINGS "${dir}/idf_component.yml" _f REGEX "^license:")
        if(_f MATCHES "^license: *[\"']?([^\"']+)")
            set(_lic "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(NOT _lic)
        set(_lic "licence not found")
    endif()
    set(${out} "${_lic}" PARENT_SCOPE)
endfunction()

# This project, at the commit it was built from.
set(_self "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${_root}"
                    OUTPUT_VARIABLE _self OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT _self)
        set(_self "unknown")
    endif()
endif()
_lv_short(_self "${_self}")
list(APPEND _libs "Lothesome Audio Analyzer|${_self}|MIT")

# The managed components, as the lock has them. "espressif/esp_lcd_touch"
# is managed_components/espressif__esp_lcd_touch, a git one by its bare name.
set(_managed "${_root}/managed_components")
if(EXISTS "${_lock}")
    _lv_depends("${_lock}")
    file(STRINGS "${_lock}" _lines)
    set(_name "")
    set(_in_deps FALSE)
    foreach(_l IN LISTS _lines)
        if(_l STREQUAL "dependencies:")
            set(_in_deps TRUE)
        elseif(_l MATCHES "^[a-z_]+:")          # direct_dependencies, target, ...
            set(_in_deps FALSE)
        elseif(_in_deps AND _l MATCHES "^  ([^ ][^:]*):$")
            set(_name "${CMAKE_MATCH_1}")
        elseif(_in_deps AND _name AND _l MATCHES "^    version: '?([^']*)'?$")
            # four spaces: the component's own version, not a requirement
            set(_ver "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "^espressif/" "" _short "${_name}")
            if(_short STREQUAL "idf")
                set(_lic "Apache-2.0")
                if(DEFINED ENV{IDF_PATH})
                    _lv_license(_lic "$ENV{IDF_PATH}")
                endif()
                list(APPEND _libs "ESP-IDF|${_ver}|${_lic}")
            elseif(NOT _short STREQUAL "cmake_utilities")
                _lv_short(_ver "${_ver}")
                string(REPLACE "/" "__" _dir "${_name}")
                _lv_license(_lic "${_managed}/${_dir}")
                list(APPEND _libs "${_short}|${_ver}|${_lic}")
            endif()
            set(_name "")
        endif()
    endforeach()
else()
    list(APPEND _libs "dependencies.lock|missing|")
endif()

# The Ark Pixel font, compiled in through feckless_graphics_handler: its
# LICENSE-OFL has to travel with the firmware.
set(_ark "${_managed}/feckless_graphics_handler/tools/gen_ark12.py")
if(EXISTS "${_ark}")
    _lv_depends("${_ark}")
    file(STRINGS "${_ark}" _v REGEX "^ARK_COMMIT *=")
    string(REGEX REPLACE "^ARK_COMMIT *= *\"([0-9a-f]+)\".*$" "\\1" _v "${_v}")
    _lv_short(_v "${_v}")
    list(APPEND _libs "Ark Pixel font (TakWolf)|${_v}|OFL-1.1")
else()
    list(APPEND _libs "Ark Pixel font (TakWolf)|-|OFL-1.1")
endif()

list(LENGTH _libs _n)
set(_h "/* Generated by cmake/libversions.cmake. Do not edit. */\n")
string(APPEND _h "#pragma once\n#define LIBVERSIONS_COUNT (${_n})\n")
string(APPEND _h "/* module, checkout, licence */\n")
string(APPEND _h "static const char *const k_libversions[LIBVERSIONS_COUNT][3] = {\n")
foreach(_l IN LISTS _libs)
    string(REPLACE "|" "\", \"" _l "${_l}")
    string(APPEND _h "    { \"${_l}\" },\n")
endforeach()
string(APPEND _h "};\n")

set(LIBVERSIONS_DIR "${CMAKE_BINARY_DIR}/libversions")
file(WRITE "${LIBVERSIONS_DIR}/libversions.h.tmp" "${_h}")
configure_file("${LIBVERSIONS_DIR}/libversions.h.tmp"
               "${LIBVERSIONS_DIR}/libversions.h" COPYONLY)
message(STATUS "libversions: ${_n} entries for the licences page")
