# components/fonts — 中文字库组件 (从 menu 拆出)
# font_zh(24px 粗) / font_zh_light(24px 细) / font_zh16(16px 宋体), 均 GB2312 全量二进制
idf_component_register(
    SRCS "font_zh.c"
         "font_zh16.c"
         "font_ascii.c"
    INCLUDE_DIRS "."
)

# 字库二进制嵌入 (XIP, 符号 _binary_*_start/end 供 font_zh.c/font_zh16.c 引用)
target_add_binary_data(${COMPONENT_LIB} "font_zh.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "font_zh_light.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "font_zh16.bin" BINARY)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
