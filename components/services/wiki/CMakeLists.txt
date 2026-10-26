# components/services/wiki — 离线维基数据服务（L3 services）
# 只做「定位→读块→LZ4→抽文本→折行」，不画像素、不依赖 ui/apps。
idf_component_register(
    SRCS "wiki_db.c" "lz4_dec.c"
    INCLUDE_DIRS "."
    REQUIRES fatfs heap esp_hw_support log
)
