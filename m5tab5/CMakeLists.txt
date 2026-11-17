cmake_minimum_required(VERSION 3.16)
# u-lothesome-0008: CMake 4.5's policy CMP0225 (ExternalProject passes
# CMAKE_INSTALL_PREFIX=<INSTALL_DIR>), NEW, for ESP-IDF's own
# ExternalProject calls -- the bootloader's -- which otherwise warn at every
# configure. Set before project.cmake: its functions keep the policies of
# its cmake_minimum_required(), which takes the default from here. Older
# CMake ignores it; nothing is installed either way.
set(CMAKE_POLICY_DEFAULT_CMP0225 NEW)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lothesome_audio_analyzer_for_m5tab5)

# 0037: libversions.h moved to elf/CMakeLists.txt with analyzer.c, which
# is built there.
