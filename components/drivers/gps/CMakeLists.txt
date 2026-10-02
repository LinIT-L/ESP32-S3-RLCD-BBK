idf_component_register(
    SRCS "gps.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_uart
        esp_driver_gpio
)
