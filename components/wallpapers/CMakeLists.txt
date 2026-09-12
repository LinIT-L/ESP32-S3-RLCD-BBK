# components/wallpapers — 内置壁纸程序渲染库
# 从 menu 组件拆出, 供 menu/桌面与 os/壁纸页 共用, 不依赖 menu 内部.
idf_component_register(
    SRCS "wallpapers.c"
    INCLUDE_DIRS "."
    REQUIRES st7305 ui_common
       PRIV_REQUIRES nvs_flash bt_ctl audio_player board_shim input os
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
