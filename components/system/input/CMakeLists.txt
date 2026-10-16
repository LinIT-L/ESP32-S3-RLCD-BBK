idf_component_register(
    SRCS "input.c"
         "gesture_back.c"
    INCLUDE_DIRS "."
    REQUIRES
        board_cfg driver
        st7305
        touch_panel
        web_gamepad
        virtual_keys
        bt_ctl
        vibrator
        tone_player
)