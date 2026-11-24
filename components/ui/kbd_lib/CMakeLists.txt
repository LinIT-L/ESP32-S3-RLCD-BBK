# components/kbd_lib — 标准 52 键虚拟键盘组件 (从 menu 拆出)
# 复用 ui_common 的 ASCII 字形 (FONT8X12) 与 st7305 绘制
idf_component_register(
    SRCS "keyboard.c"
         "kbd_ui.c"
         "kbd_c42.c"       # 经典 42 键紧凑键盘 (弹窗版/页内版共用一份)
         "ime.c"
         "ime_pinyin_data.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES fonts ui_common
    REQUIRES
        st7305
    )

target_compile_options(${COMPONENT_LIB} PRIVATE
)
