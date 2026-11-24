idf_component_register(SRCS "vibrator.c"
                       INCLUDE_DIRS "."
    PRIV_REQUIRES audio_player board_pins
                       REQUIRES
        driver esp_driver_gpio esp_driver_ledc
    )