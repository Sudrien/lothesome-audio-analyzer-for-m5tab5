# 0037: one directory, two builds. elf/ sets LOTHESOME_ELF and builds
# analyzer.c into the ELF, and checks it against the feckless ABI (0039);
# the top level builds standalone.c, runs elf/ and embeds what it made.
idf_build_get_property(_lothesome_elf LOTHESOME_ELF)

if(_lothesome_elf)
    idf_component_register(
        SRCS "analyzer.c"
        INCLUDE_DIRS "."
        REQUIRES feckless_abi feckless_drivers feckless_graphics_handler
                 esp-dsp esp_timer
                 esp_app_format   # 0005: esp_app_get_description()
    )
    return()
endif()

idf_component_register(
    SRCS "standalone.c"
    PRIV_REQUIRES feckless_abi
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(ExternalProject)
    idf_build_get_property(_idf_path IDF_PATH)
    idf_build_get_property(_target IDF_TARGET)
    idf_build_get_property(_build_dir BUILD_DIR)
    idf_build_get_property(_sdkconfig SDKCONFIG)
    idf_build_get_property(_project_dir PROJECT_DIR)
    idf_build_get_property(_project_ver PROJECT_VER)

    set(_elf_dir "${_build_dir}/elf")
    set(_elf "${_elf_dir}/lothesome_analyzer.app.elf")

    externalproject_add(lothesome_elf
        SOURCE_DIR "${_project_dir}/elf"
        BINARY_DIR "${_elf_dir}"
        CMAKE_ARGS -DIDF_PATH=${_idf_path}
                   -DIDF_TARGET=${_target}
                   -DSDKCONFIG=${_elf_dir}/sdkconfig
                   -DLOTHESOME_HOST_SDKCONFIG=${_sdkconfig}
                   -DLOTHESOME_PROJECT_VER=${_project_ver}   # 0046
        CMAKE_GENERATOR "Unix Makefiles"
        BUILD_COMMAND make elf
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${_elf}"
        INSTALL_COMMAND ""
    )

    # elf/ checked it against the ABI as it linked it (0039); embedding
    # waits for that.
    add_dependencies(${COMPONENT_LIB} lothesome_elf)

    # elf_loader's: 4-byte aligned, as esp_elf_relocate() wants it.
    elf_embed_aligned_binary(${COMPONENT_LIB} "${_elf}" BINARY)
endif()
