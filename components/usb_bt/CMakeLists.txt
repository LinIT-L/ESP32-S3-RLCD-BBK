idf_component_register(
    SRCS "usb_bt.c" "usb_bt_bth.c"
    INCLUDE_DIRS "include"
    REQUIRES tinyusb usb_hid bt_ctl nvs_flash
    PRIV_REQUIRES esp_tinyusb bt usb
)