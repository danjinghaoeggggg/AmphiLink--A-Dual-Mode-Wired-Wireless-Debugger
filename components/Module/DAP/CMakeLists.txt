# Project-specific ESP32-S3 integration for the upstream CMSIS-DAP sources.
idf_component_register(
    SRCS
        "src/DAP.c"
        "src/SW_DP.c"
        "src/JTAG_DP.c"
        "src/dap_port.c"
    INCLUDE_DIRS
        "inc"
    REQUIRES
        BSP
        soc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
