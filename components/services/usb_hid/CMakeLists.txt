idf_component_register(
    SRCS "usb_hid.c"
    INCLUDE_DIRS "include"
    REQUIRES tinyusb driver esp_driver_usb_serial_jtag
    PRIV_REQUIRES esp_tinyusb
)