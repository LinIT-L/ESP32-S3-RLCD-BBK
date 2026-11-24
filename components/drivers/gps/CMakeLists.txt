idf_component_register(
    LDFRAGMENTS "linker.lf"
    SRCS "gps.c" "gps_nmea.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board_pins osutil
    REQUIRES
        driver
        esp_driver_uart
        esp_driver_gpio
        esp_timer
    )
