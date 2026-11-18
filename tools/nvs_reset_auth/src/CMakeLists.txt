# nvs_reset_auth component — one source file.
# nvs_flash              nvs_flash_init, nvs_open, nvs_get_str, nvs_erase_key,
#                         nvs_commit, ESP_ERR_NVS_* result codes
# esp_driver_uart        uart_driver_install / uart_read_bytes on UART0
#                         (stdin's simple VFS read reports "no data" as EOF on
#                         this IDF, so console input goes through the driver
#                         explicitly — same reason as sd_format/nvs_provision)
# esp_partition           esp_partition_find_first — prints the nvs view this
#                         tool actually boots with, for the evidence log
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_driver_uart esp_partition
)
