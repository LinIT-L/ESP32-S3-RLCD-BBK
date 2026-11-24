idf_component_register(
    SRCS "hw_cfg.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board_pins
    REQUIRES
        driver esp_driver_i2c nvs_flash esp_driver_gpio
    )
