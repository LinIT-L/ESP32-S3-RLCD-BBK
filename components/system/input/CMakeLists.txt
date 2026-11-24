idf_component_register(
    LDFRAGMENTS "linker.lf"
    SRCS "input.c"
         "gesture_back.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board_pins bt_ctl hw_cfg power_mgr st7305 tone_player vibrator virtual_keys web_gamepad
    REQUIRES
    driver
    esp_driver_gpio
    touch_panel
    )