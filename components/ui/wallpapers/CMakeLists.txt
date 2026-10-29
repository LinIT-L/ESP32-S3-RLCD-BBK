# components/wallpapers — 内置壁纸程序渲染库
# 从 menu 组件拆出, 供 menu/桌面与 os/壁纸页 共用, 不依赖 menu 内部.
idf_component_register(
    SRCS "wallpapers.c" "wp_sleep.c" "wp_module_stars.c" "wp_module_lifeguide.c" "power_stat.c"
    INCLUDE_DIRS "."
    REQUIRES st7305 ui_common
    board_pins
       # V1.2 模块化: 移除陈旧的 os 依赖 (wallpapers 未 include 任何 os 头) — 破 os ⇄ wallpapers 循环
       # ★ 2026-10-28 (第 5 阶段 Step 3): 电源编排迁到 system/power_mgr ⇒
       #   bt_ctl / audio_player / gps / touch_panel 已从本组件依赖里移除 (只有 sleep.c 用过它们)
       PRIV_REQUIRES nvs_flash svc_nvs board_shim input wifi_manager usb_hid
                      esp_hw_support
       # usb_hid: usb_hid_host_mounted() —— "USB 连接时不自动休眠" (2026-10-02) / power_stat 的充电判定
       # esp_hw_support: power_stat.c 的 esp_rtc_get_time_us() (RTC 计时, 跨深睡)
)

target_compile_options(${COMPONENT_LIB} PRIVATE
)
