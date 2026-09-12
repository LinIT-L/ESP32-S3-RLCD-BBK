idf_component_register(
    SRCS "audio_player.c" "es8311.c" "es7210.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2s esp_driver_i2c fatfs
)
