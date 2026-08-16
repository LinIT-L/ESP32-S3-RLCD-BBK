idf_component_register(
    SRCS "app_board.c"
    INCLUDE_DIRS "." "include"
    REQUIRES
        st7305
        menu
        input
        nvs_flash
        esp_event
        system_rom
        audio_player
        board_battery
        driver
)
