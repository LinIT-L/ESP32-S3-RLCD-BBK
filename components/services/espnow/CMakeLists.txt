# components/services/espnow — ESP-NOW 点对点/广播 (2026-10-20)
# 与 wifi_manager 共用同一个 WiFi 驱动: 本组件只负责 esp_now_* 这一层。
idf_component_register(
    SRCS "espnow_link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi freertos
    PRIV_REQUIRES wifi_manager esp_event
)
