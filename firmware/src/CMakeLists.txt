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
# esp_wifi/esp_netif/esp_event  wifi_bringup.cpp (Phase 0 smoke: SoftAP +
#                scan only, no credentials) and network/network_manager.cpp
#                (station mode + NTP via esp_netif's SNTP service, plan 02
#                task 1.1.2). SNTP needs no extra component on IDF 6.1.
# nvs_flash      Wi-Fi calibration storage required by esp_wifi_init
# esp_timer      core/resource_monitor.cpp: esp_timer_get_time() for uptime,
#                event timestamps and monitor sampling intervals
idf_component_register(
    SRCS
        "main.cpp"
        "sd_bringup.cpp"
        "wifi_bringup.cpp"
        "core/core_init.cpp"
        "core/event_bus.cpp"
        "core/resource_monitor.cpp"
        "core/ring_log.cpp"
        "network/network_manager.cpp"
        "network/wifi_credentials.cpp"
        "camera/camera_manager.cpp"
        "camera/camera_recovery.cpp"
        "frame/frame_manager.cpp"
        "auth/auth.cpp"
        "web/web_server.cpp"
        "storage/storage_manager.cpp"
        "storage/sd_storage_backend.cpp"
        "snapshot/snapshot_manager.cpp"
        "recording/avi_writer.cpp"
        "recording/recording_manager.cpp"
        "recording/playback.cpp"
        "web_assets/web_asset_0.c"
        "web_assets/web_asset_1.c"
        "web_assets/web_asset_2.c"
        "web_assets/web_asset_3.c"
        "web_assets/web_asset_4.c"
        "web_assets/web_assets_manifest.c"
    INCLUDE_DIRS
        "../include"
        "web_assets"
        "recording"
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
        esp_timer
        # mbedtls: auth.cpp PBKDF2-HMAC-SHA256 + SHA-256 token hashing
        mbedtls
)
