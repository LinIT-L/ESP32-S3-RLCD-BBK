idf_component_register(SRCS "vibrator.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver audio_player board_pins esp_driver_gpio esp_driver_ledc)