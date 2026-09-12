# components/keyvault — 密钥管理器 加密存储
idf_component_register(
    SRCS "keyvault.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mbedtls esp_timer heap
)