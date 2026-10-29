cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lothesome_audio_analyzer_for_m5tab5)

# 0037: libversions.h moved to elf/CMakeLists.txt with analyzer.c, which
# is built there.
