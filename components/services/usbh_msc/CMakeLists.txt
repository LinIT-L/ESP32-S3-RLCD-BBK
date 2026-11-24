idf_component_register(
    SRCS "usbh_msc_sdspi.c"
    INCLUDE_DIRS "include"
    REQUIRES
        tinyusb sdmmc driver
        sd_scan            # ★ 2026-11-24: sd_get_card() / sd_arb_state() —— 交卡前后校验
    PRIV_REQUIRES esp_tinyusb

        svc_log usb_hid)
