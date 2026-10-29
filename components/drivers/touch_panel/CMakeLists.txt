idf_component_register(
    SRCS "touch_panel.c"
    INCLUDE_DIRS "."
    REQUIRES
        board_pins driver nvs_flash esp_driver_gpio esp_driver_i2c
)
