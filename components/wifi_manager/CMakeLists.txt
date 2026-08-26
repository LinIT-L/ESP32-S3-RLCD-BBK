# components/wifi_manager — Wi-Fi STA 连接管理 (独立组件)
# 从 menu 拆出, 供 menu/os 页面共用.
idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
