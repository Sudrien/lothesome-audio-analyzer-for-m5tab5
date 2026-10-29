# 0037: the analyzer as an ELF for esp_elf -- main/analyzer.c, the same
# file the .bin runs, linked position-independent with every library
# call left undefined. The libraries are components/lothesome_abi's.
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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Tells main/CMakeLists.txt which of its two builds this is.
idf_build_set_property(LOTHESOME_ELF 1)
# One manifest, so one lock: the top level's.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/../dependencies.lock")

project(lothesome_analyzer)

include(elf_loader)
project_elf(lothesome_analyzer)

# 0036: libversions.h, which analyzer.c includes. Moved here from the
# top-level CMakeLists.txt with analyzer.c.
idf_component_get_property(main_lib main COMPONENT_LIB)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/libversions.cmake)
target_include_directories(${main_lib} PRIVATE "${LIBVERSIONS_DIR}")
