# components/wallpapers — 内置壁纸程序渲染库
# 从 menu 组件拆出, 供 menu/桌面与 os/壁纸页 共用, 不依赖 menu 内部.
idf_component_register(
    SRCS "wallpapers.c" "wp_sleep.c" "wp_module_stars.c" "wp_module_lifeguide.c" "power_stat.c"
    INCLUDE_DIRS "."
    # st7305: wallpapers.h 的公共接口暴露 st7305_handle_t ⇒ 必须公开
    REQUIRES st7305
    # V1.2 模块化: 移除陈旧的 os 依赖 (wallpapers 未 include 任何 os 头) — 破 os ⇄ wallpapers 循环
    # ★ 2026-10-28 (第 5 阶段 Step 3): 电源编排迁到 system/power_mgr ⇒
    #   bt_ctl / audio_player / gps / touch_panel 已从本组件依赖里移除 (只有 sleep.c 用过它们)
    PRIV_REQUIRES
        board_pins ui_common
        nvs_flash svc_nvs board_shim input wifi_manager usb_hid esp_driver_usb_serial_jtag
        audio_player   # ★ 2026-11-22: audio_player_get_state()/rec_active() —— 播放/录音中不自动息屏/关机
        # ★ 2026-11-24 省电模式: 闲置关无线电的唯一判定点在 wallpapers.c
        #   net_client → netc_busy(): 下载/上传/商店/OTA/Lua 网络任务不算"无操作"
        #   bt_ctl     → bt_manager_is_busy() / bt_manager_release_async()
        net_client bt_ctl
        esp_hw_support
        # usb_hid: usb_hid_host_mounted() —— "USB 连接时不自动休眠" (2026-10-02) / power_stat 的充电判定
        # esp_hw_support: power_stat.c 的 esp_rtc_get_time_us() (RTC 计时, 跨深睡)
)

target_compile_options(${COMPONENT_LIB} PRIVATE
)
