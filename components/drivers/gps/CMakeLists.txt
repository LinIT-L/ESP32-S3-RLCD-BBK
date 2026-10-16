idf_component_register(
    SRCS "gps.c" "gps_nmea.c"
    INCLUDE_DIRS "."
    REQUIRES
        board_pins driver
        esp_driver_uart
        esp_driver_gpio
        esp_timer
)
