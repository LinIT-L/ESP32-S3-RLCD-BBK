idf_component_register(
    SRCS "app_board.c"
    INCLUDE_DIRS "." "include"
    REQUIRES
        hw_cfg board_pins st7305
        sd_scan
        input
        nvs_flash svc_nvs
        esp_event
        audio_player
        board_battery
        gps
        bt_ctl
        usb_bt
        usb_net
        driver esp_driver_gpio
)
