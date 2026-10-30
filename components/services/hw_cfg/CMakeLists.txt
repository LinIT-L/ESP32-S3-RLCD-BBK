idf_component_register(
    SRCS "hw_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c nvs_flash board_pins esp_driver_gpio
)
