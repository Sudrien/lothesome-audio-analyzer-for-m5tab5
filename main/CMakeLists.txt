# 0037: one directory, two builds. elf/ sets LOTHESOME_ELF and builds
# analyzer.c into the ELF; the top level builds standalone.c, runs elf/,
# checks what it made against the ABI and embeds it.
idf_build_get_property(_lothesome_elf LOTHESOME_ELF)

if(_lothesome_elf)
    idf_component_register(
        SRCS "analyzer.c"
        INCLUDE_DIRS "."
        REQUIRES lothesome_abi feckless_drivers feckless_graphics_handler
                 esp-dsp esp_timer
                 esp_app_format   # 0005: esp_app_get_description()
    )
    return()
endif()

idf_component_register(
    SRCS "standalone.c"
    PRIV_REQUIRES lothesome_abi
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(ExternalProject)
    idf_build_get_property(_idf_path IDF_PATH)
    idf_build_get_property(_target IDF_TARGET)
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_sdkconfig SDKCONFIG)
    idf_build_get_property(_project_dir PROJECT_DIR)

    set(_elf_dir "${_build_dir}/elf")
    set(_elf "${_elf_dir}/lothesome_analyzer.app.elf")

    externalproject_add(lothesome_elf
        SOURCE_DIR "${_project_dir}/elf"
        BINARY_DIR "${_elf_dir}"
        CMAKE_ARGS -DIDF_PATH=${_idf_path}
                   -DIDF_TARGET=${_target}
                   -DSDKCONFIG=${_elf_dir}/sdkconfig
                   -DLOTHESOME_HOST_SDKCONFIG=${_sdkconfig}
        CMAKE_GENERATOR "Unix Makefiles"
        BUILD_COMMAND make elf
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${_elf}"
        INSTALL_COMMAND ""
    )

    string(REPLACE "-gcc" "-readelf" _readelf "${CMAKE_C_COMPILER}")
    set(_stamp "${_elf_dir}/abicheck.stamp")
    add_custom_command(
        OUTPUT "${_stamp}"
        COMMAND ${CMAKE_COMMAND} -DREADELF=${_readelf} -DELF=${_elf}
                -DEXPORTS=${_project_dir}/components/lothesome_abi/exports.txt
                -DSTAMP=${_stamp}
                -P ${_project_dir}/cmake/abicheck.cmake
        DEPENDS "${_elf}" lothesome_elf
                "${_project_dir}/components/lothesome_abi/exports.txt"
        COMMENT "Checking the ELF against exports.txt"
    )
    add_custom_target(lothesome_abicheck DEPENDS "${_stamp}")
    add_dependencies(${COMPONENT_LIB} lothesome_abicheck)

    # elf_loader's: 4-byte aligned, as esp_elf_relocate() wants it.
    elf_embed_aligned_binary(${COMPONENT_LIB} "${_elf}" BINARY)
endif()
