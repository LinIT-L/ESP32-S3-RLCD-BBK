# favorites — 游戏收藏管理 (按引擎独立, appdata 分区持久化)
# 从 menu 组件抽离: os 内核/页面依赖收藏, 独立成组件便于复用与裁剪
idf_component_register(
    SRCS "favorites.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition heap esp_common log
)
