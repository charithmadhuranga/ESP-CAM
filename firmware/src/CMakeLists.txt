# VisionCam application component.
# ESP-IDF builds every listed source; keep this list explicit (src_filter is
# unavailable under framework=espidf).
#
# PRIV_REQUIRES: IDF auto-adds only its "common requirements" (esp_system,
# heap, freertos, log, ...) to every component. Because this component is
# named `src` rather than `main`, it gets NO extra automatic dependencies —
# the PlatformIO ESP-IDF docs call this out explicitly. Anything we include
# beyond the common set must be listed here or the link fails with an
# "undefined reference" (first observed on esp_psram_get_size, 2026-11-15).
#
# esp_psram      esp_psram_get_size() — its object is only compiled under
#                CONFIG_SPIRAM (ADR-0001).
# esp32-camera   R2 driver, pinned 2.1.9 in idf_component.yml. The component
#                name is the registry path with '/' -> '_' (rule 33's check in
#                the root CMakeLists.txt confirms exactly one copy exists).
# fatfs          esp_vfs_fat_sdmmc_mount / unmount
# sdmmc          sdmmc_card_print_info, sdmmc_card_t
# esp_driver_sdmmc  driver/sdmmc_host.h, SDMMC_HOST_DEFAULT, slot config
# esp_driver_gpio   driver/gpio.h (GPIO4 lamp, PWDN power-cycle)
# esp_wifi/esp_netif/esp_event  wifi_bringup.cpp (step 14: camera+SD under
#                Wi-Fi off then on; SoftAP + scan only, no credentials)
# nvs_flash      Wi-Fi calibration storage required by esp_wifi_init
idf_component_register(
    SRCS
        "main.cpp"
        "camera_bringup.cpp"
        "sd_bringup.cpp"
        "wifi_bringup.cpp"
    INCLUDE_DIRS "../include"
    PRIV_REQUIRES
        esp_psram
        espressif__esp32-camera
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp_driver_gpio
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
)
