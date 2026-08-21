# Main 组件
# BBK 游戏模拟器 - 菜单系统前端

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        self_test
        input
        app_board
        st7305
        menu
        driver
        esp_timer
        nvs_flash
        esp_event
        board_battery
        book_reader
        tone_player
        vibrator
        engine_manager
)
