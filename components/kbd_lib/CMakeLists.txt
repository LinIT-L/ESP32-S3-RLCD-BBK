# components/kbd_lib — 标准 52 键虚拟键盘组件 (从 menu 拆出)
# 复用 ui_common 的 ASCII 字形 (FONT8X12) 与 st7305 绘制
idf_component_register(
    SRCS "keyboard.c"
    INCLUDE_DIRS "."
    REQUIRES st7305 ui_common fonts
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
