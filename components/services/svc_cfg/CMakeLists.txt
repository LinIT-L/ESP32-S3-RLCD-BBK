# components/svc_cfg — svc_nvs 便捷薄封装 (按 namespace+key 即开即读写, 带缺省值)
idf_component_register(
    SRCS "svc_cfg.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash svc_nvs esp_common
)
