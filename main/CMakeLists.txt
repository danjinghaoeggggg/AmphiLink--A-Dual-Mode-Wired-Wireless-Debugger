idf_component_register(
    SRCS
        "src/main.c"
    INCLUDE_DIRS
        "inc"
    REQUIRES
        amphilink
)

set(WIFI_NVS_CSV_PATH "${CMAKE_CURRENT_LIST_DIR}/../wifi_nvs.csv")
file(GLOB WIFI_NVS_CSV CONFIGURE_DEPENDS "${WIFI_NVS_CSV_PATH}")
if(WIFI_NVS_CSV)
    nvs_create_partition_image(wifi_nvs "${WIFI_NVS_CSV_PATH}"
                               FLASH_IN_PROJECT)
else()
    message(STATUS "wifi_nvs.csv not found; skipping Wi-Fi NVS image")
endif()

set(IDENTITY_NVS_CSV_PATH "${CMAKE_CURRENT_LIST_DIR}/../identity_nvs.csv")
file(GLOB IDENTITY_NVS_CSV CONFIGURE_DEPENDS "${IDENTITY_NVS_CSV_PATH}")
if(IDENTITY_NVS_CSV)
    nvs_create_partition_image(identity_nvs "${IDENTITY_NVS_CSV_PATH}"
                               FLASH_IN_PROJECT)
else()
    message(STATUS
            "identity_nvs.csv not found; using the default device identity")
endif()

fatfs_create_rawflash_image(
    webpage_vfs
    "${CMAKE_CURRENT_LIST_DIR}/../web"
    FLASH_IN_PROJECT
)
