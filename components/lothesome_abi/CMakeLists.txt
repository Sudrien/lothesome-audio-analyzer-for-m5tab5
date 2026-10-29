# 0037: the host side of the analyzer's ELF -- the libraries, and the
# table that hands them to esp_elf. A program that wants to run the
# published ELF (a launcher) adds this component, by git with
# path: components/lothesome_abi, at the release's tag.
#
# exports.txt is the ABI: every symbol the ELF leaves undefined, one a
# line. The table is generated from it, and the ELF build checks its
# undefined symbols against it (cmake/abicheck.cmake), so a change to
# what analyzer.c calls fails the build until exports.txt says so.
#
# The table names each symbol, which is also what makes the linker keep
# it: nothing in the host calls gfx_blit(), only the ELF does.
#
# SPDX-License-Identifier: MIT

set(_exports "${CMAKE_CURRENT_LIST_DIR}/exports.txt")
set(_table "${CMAKE_CURRENT_BINARY_DIR}/lothesome_abi_syms.c")

idf_component_register(
    SRCS "lothesome_abi.c" "${_table}"
    INCLUDE_DIRS "include"
    REQUIRES elf_loader
             feckless_drivers feckless_graphics_handler esp-dsp
             esp_timer esp_app_format
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_exports}")
    file(STRINGS "${_exports}" _syms REGEX "^[A-Za-z_][A-Za-z0-9_]*$")
    # As tool/symbols.py writes it: the type does not matter, only the
    # address, and "int" for memcpy needs the warning off.
    set(_c "/* Generated from exports.txt by CMakeLists.txt. Do not edit. */\n")
    string(APPEND _c "#include <stddef.h>\n#include \"private/elf_symbol.h\"\n\n")
    string(APPEND _c "#pragma GCC diagnostic push\n")
    string(APPEND _c "#pragma GCC diagnostic ignored \"-Wbuiltin-declaration-mismatch\"\n")
    foreach(_s IN LISTS _syms)
        string(APPEND _c "extern int ${_s};\n")
    endforeach()
    string(APPEND _c "#pragma GCC diagnostic pop\n\n")
    string(APPEND _c "const struct esp_elfsym g_lothesome_abi_elfsyms[] = {\n")
    foreach(_s IN LISTS _syms)
        string(APPEND _c "    ESP_ELFSYM_EXPORT(${_s}),\n")
    endforeach()
    string(APPEND _c "    ESP_ELFSYM_END\n};\n")
    file(WRITE "${_table}.tmp" "${_c}")
    configure_file("${_table}.tmp" "${_table}" COPYONLY)
endif()
