idf_component_register(SRCS "usb_net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash svc_log svc_nvs usb_hid esp_system esp_tinyusb esp_wifi esp_event wifi_manager)