idf_component_register(
    SRCS "touch_panel.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board_pins
    REQUIRES
        driver nvs_flash esp_driver_gpio esp_driver_i2c
    )
