# components/svc_log — 日志服务 (缓存/落盘/串口暂停)
idf_component_register(
    SRCS "svc_log.c"
    INCLUDE_DIRS "include"
    REQUIRES sd_scan freertos esp_common
)
