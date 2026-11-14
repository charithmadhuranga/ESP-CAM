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
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "../include"
    PRIV_REQUIRES esp_psram
)
