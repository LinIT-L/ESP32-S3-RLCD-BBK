idf_component_register(
    SRCS "usb_hid.c"
    INCLUDE_DIRS "include"
    REQUIRES tinyusb driver
    PRIV_REQUIRES esp_tinyusb usb
)