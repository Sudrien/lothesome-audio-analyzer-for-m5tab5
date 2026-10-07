idf_component_register(
    SRCS "analyzer.c"
    INCLUDE_DIRS "."
    REQUIRES feckless_drivers feckless_graphics_handler esp-dsp esp_timer
)
