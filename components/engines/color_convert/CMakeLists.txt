# color_convert — 通用色深转换 (NES/GBC 共用)
idf_component_register(
    SRCS "color_convert.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common log
)
