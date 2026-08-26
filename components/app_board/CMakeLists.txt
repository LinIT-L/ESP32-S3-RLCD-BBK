idf_component_register(
    SRCS "app_board.c"
    INCLUDE_DIRS "." "include"
    REQUIRES
        st7305
        menu
        sd_scan
        input
        nvs_flash
        esp_event
        audio_player
        board_battery
        driver
)
