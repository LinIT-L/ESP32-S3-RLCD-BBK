# components/ui_common — 通用绘制内核 (纯绘制原语 + 中英文字体渲染 + 位图图标).
# 从 menu_system.c 拆出: 不依赖 menu_state_t, 只依赖 st7305 显示驱动 + 中文字库(fonts).
# 是 components/os 与各应用页模块共享的最小绘图依赖.
#
# 依赖 menu 组件仅取其位图图标资源 (icons_main.inc/cat_icons.inc);
# 字库来自 components/fonts; 方向为 ui_common -> menu/fonts (单向, 无循环).
idf_component_register(
    SRCS "ui_common.c"
    INCLUDE_DIRS "."
    REQUIRES st7305 fonts
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
