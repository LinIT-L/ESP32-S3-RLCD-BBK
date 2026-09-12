# components/fonts — 中文字库组件 (从 menu 拆出)
# font_zh(24px 粗) / font_zh_light(24px 细) / font_zh16(16px 宋体), 均 GB2312 全量二进制
# V1.0.89+: 字库二进制已迁移到 appdata 分区字库区 (esp_partition_mmap 运行时加载),
# 不再嵌入固件 (target_add_binary_data 已移除), 固件仅保留加载器 font_part.c
idf_component_register(
    SRCS "font_zh.c"
         "font_zh16.c"
         "font_ascii.c"
         "font_part.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition heap
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
