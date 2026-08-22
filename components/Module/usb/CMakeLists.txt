idf_component_register(
    SRCS
        "src/usb.c"
    INCLUDE_DIRS
        "inc"
    REQUIRES
        BSP
        espressif__tinyusb
        freertos
)

# Standalone TinyUSB requires application-provided tusb_config.h
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(bsp_dir BSP COMPONENT_DIR)
target_include_directories(${tusb_lib} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/inc"
    "${bsp_dir}/inc"
)
target_sources(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/usb_descriptors.c")
