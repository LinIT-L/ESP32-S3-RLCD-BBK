idf_component_register(SRCS "usb_net.c"
                       INCLUDE_DIRS "include"
    PRIV_REQUIRES svc_log svc_nvs usb_hid wifi_manager
                       REQUIRES
        nvs_flash esp_system esp_tinyusb esp_wifi esp_event
    )