idf_component_register(
    SRCS "web_gamepad.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_server
        esp_wifi
        esp_netif
        esp_event
        nvs_flash svc_nvs
        board_battery
        wifi_manager
)
