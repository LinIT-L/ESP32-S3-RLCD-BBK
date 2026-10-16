# components/svc_time — 时间/校时统一服务
idf_component_register(
    SRCS "svc_time.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip nvs_flash svc_nvs
)
