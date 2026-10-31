# 0037: the analyzer as an ELF for esp_elf -- main/analyzer.c, the same
# file the .bin runs, linked position-independent with every library
# call left undefined but esp-dsp's, which is linked in. The rest is
# the feckless ABI's (feckless_abi from feckless-libs-for-m5tab5, 0039,
# 0047), and the build
# checks the ELF against it.
#
# The top-level build runs this one (main/CMakeLists.txt) and embeds
# what it makes. On its own:
#   idf.py -C elf -B build-elf build
# writes build-elf/lothesome_analyzer.app.elf.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

# The configuration is the .bin's, copied whole when the top-level
# build passes it: struct layouts and inline functions in the headers
# depend on it, and the ELF must agree with the host it runs on. On its
# own, the defaults.
#
# Except partitions.csv, which is relative to the project and so would
# be looked for in elf/: made absolute, here and in the defaults (the
# later file wins).
set(_partitions "${CMAKE_CURRENT_LIST_DIR}/../partitions.csv")
get_filename_component(_partitions "${_partitions}" ABSOLUTE)
set(_partitions_line "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_partitions}\"")
if(LOTHESOME_HOST_SDKCONFIG AND SDKCONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LOTHESOME_HOST_SDKCONFIG}")
    file(READ "${LOTHESOME_HOST_SDKCONFIG}" _cfg)
    string(REGEX REPLACE "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"[^\"\n]*\""
           "${_partitions_line}" _cfg "${_cfg}")
    file(WRITE "${SDKCONFIG}.host" "${_cfg}")
    configure_file("${SDKCONFIG}.host" "${SDKCONFIG}" COPYONLY)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults" "${_partitions_line}\n")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.defaults"
                       "${CMAKE_BINARY_DIR}/sdkconfig.elf.defaults")

# main is ../main, and only what it needs is built.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../main")
set(COMPONENTS main)

# 0046: the .bin's version, which CI takes from the release tag
# (version.txt beside the .bin's CMakeLists.txt, not here), so the ELF's
# own esp_app_desc_t -- feckless-abi v0.2.5 links it in -- says the same.
# On its own, IDF's usual: git describe.
if(LOTHESOME_PROJECT_VER)
    set(PROJECT_VER "${LOTHESOME_PROJECT_VER}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Tells main/CMakeLists.txt which of its two builds this is.
idf_build_set_property(LOTHESOME_ELF 1)
# One manifest, so one lock: the top level's.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/../dependencies.lock")

project(lothesome_analyzer)

# 0039: esp-dsp into the ELF, not the ABI. Its name is
# espressif__esp-dsp from the registry; whichever it is, it is the
# component whose name ends so.
idf_build_get_property(_comps BUILD_COMPONENTS)
foreach(_c IN LISTS _comps)
    if(_c MATCHES "(^|__)esp-dsp$")
        set(ELF_COMPONENTS "${_c}")
    endif()
endforeach()
feckless_abi_elf(lothesome_analyzer)

# 0036: libversions.h, which analyzer.c includes. Moved here from the
# top-level CMakeLists.txt with analyzer.c.
idf_component_get_property(main_lib main COMPONENT_LIB)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/libversions.cmake)
target_include_directories(${main_lib} PRIVATE "${LIBVERSIONS_DIR}")
