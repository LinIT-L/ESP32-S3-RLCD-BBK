# favorites — 游戏收藏管理 (NVS 数据库持久化)
idf_component_register(
    SRCS "favorites.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition heap esp_common log nvs_flash svc_nvs
)
