# components/svc_nvs — NVS 落盘统一封装服务
idf_component_register(
    SRCS "svc_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_common
)
