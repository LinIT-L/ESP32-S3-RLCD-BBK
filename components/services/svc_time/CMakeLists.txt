# components/svc_time — 时间/校时统一服务
idf_component_register(
    SRCS "svc_time.c" "time_logic.c"   # time_logic: 纯判定(刷机识别/节流/偏差), 主机可测
    INCLUDE_DIRS "include"
    PRIV_REQUIRES svc_nvs
    REQUIRES
        esp_netif lwip nvs_flash esp_timer freertos
    )
