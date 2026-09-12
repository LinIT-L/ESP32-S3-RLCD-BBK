idf_component_register(
    SRCS "board_battery.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_adc
        log
)
