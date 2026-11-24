idf_component_register(
    SRCS "usb_bt.c" "usb_bt_bth.c"
    INCLUDE_DIRS "include"
    REQUIRES
        tinyusb nvs_flash
    PRIV_REQUIRES esp_tinyusb bt

        bt_ctl svc_nvs usb_hid)