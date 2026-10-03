# components/wallpapers — 内置壁纸程序渲染库
# 从 menu 组件拆出, 供 menu/桌面与 os/壁纸页 共用, 不依赖 menu 内部.
idf_component_register(
    SRCS "wallpapers.c" "sleep.c" "wp_module_stars.c"
    INCLUDE_DIRS "."
    REQUIRES st7305 ui_common
       # V1.2 模块化: 移除陈旧的 os 依赖 (wallpapers 未 include 任何 os 头) — 破 os ⇄ wallpapers 循环
       PRIV_REQUIRES nvs_flash svc_nvs bt_ctl audio_player board_shim input wifi_manager usb_hid
       # usb_hid: usb_hid_host_mounted() —— "USB 连接时不自动休眠" (2026-10-02)
)

target_compile_options(${COMPONENT_LIB} PRIVATE
)
