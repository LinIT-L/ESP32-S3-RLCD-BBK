idf_component_register(
    SRCS "usbh_msc_sdspi.c"
    INCLUDE_DIRS "include"
    REQUIRES tinyusb sdmmc driver
    PRIV_REQUIRES esp_tinyusb
)
