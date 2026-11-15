# nvs_provision component — one source file.
# nvs_flash       nvs_open/set_str/commit
# esp_driver_uart uart_driver_install / uart_read_bytes on UART0 (stdin's
#                 simple VFS read reports "no data" as EOF on this IDF, so
#                 console input goes through the driver explicitly)
# esp_wifi/esp_netif/esp_event  the `join` diagnostic: reproduce R1's STA
#                 connect and print the raw WIFI_REASON_* code
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_driver_uart esp_wifi esp_netif esp_event
)
