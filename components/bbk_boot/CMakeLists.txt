# bbk_boot — 开机 LOGO 位图数据 (从 st7305 驱动拆出, 显示驱动回归纯显示)
idf_component_register(
    SRCS "bbk_boot.c"
    INCLUDE_DIRS "."
    REQUIRES st7305 ui_common
)
