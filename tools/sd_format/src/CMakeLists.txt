# sd_format component — one source file.
# fatfs                   f_mkfs / f_mount / f_getfree / f_open..f_unlink and
#                         ff_diskio_register_sdmmc (diskio_sdmmc.h)
# sdmmc, esp_driver_sdmmc sdmmc_card_init, sdmmc_read_sectors,
#                         sdmmc_card_print_info, sdmmc_host_init_slot,
#                         SDMMC_HOST_DEFAULT (public requirements of fatfs,
#                         listed for clarity)
# esp_driver_uart         uart_driver_install / uart_read_bytes on UART0
#                         (stdin's simple VFS read reports "no data" as EOF
#                         on this IDF, so console input goes through the
#                         driver explicitly — same reason as nvs_provision)
# esp_timer               format duration measurement
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_uart esp_timer
)
