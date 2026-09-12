idf_component_register(
    SRCS "input.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        touch_panel
        web_gamepad
        virtual_keys
        bt_ctl
        vibrator
        tone_player
)