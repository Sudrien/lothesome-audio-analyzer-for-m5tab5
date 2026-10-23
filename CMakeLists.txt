cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lothesome_audio_analyzer_for_m5tab5)

# 0036: libversions.h, the licences page's list, generated from the lock
# when the build is configured. See the file.
idf_component_get_property(main_lib main COMPONENT_LIB)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/libversions.cmake)
target_include_directories(${main_lib} PRIVATE "${LIBVERSIONS_DIR}")
