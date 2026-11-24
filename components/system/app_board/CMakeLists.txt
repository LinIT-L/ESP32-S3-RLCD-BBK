idf_component_register(
    SRCS "app_board.c"
    INCLUDE_DIRS "." "include"
    PRIV_REQUIRES audio_player board_battery board_pins bt_ctl gps hw_cfg input sd_scan svc_nvs usb_bt usb_net
    REQUIRES
        st7305
        nvs_flash
        esp_event
        driver
        esp_driver_gpio
    )
