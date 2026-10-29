# components/system/power_mgr — 系统电源管理 (深睡/关机流程的唯一实现)
#
# 2026-10-28 (第 5 阶段 Step 3): 从 components/ui/wallpapers/sleep.c 迁出。
# 迁出前 wallpapers(UI 层) 直接编排 bt/wifi/音频/GPS/SD/触摸/屏 的关闭顺序,
# 连 system/input 都要 extern 它取按键初始化 —— 边界是错的。
#
# 本组件不依赖任何 UI 组件: 关机画面由调用方(wallpapers/sleep.c)先刷,
# 电池统计由 power_stat 通过 power_mgr_set_pre_sleep_hook() 挂进来。
idf_component_register(
    SRCS "power_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES board_pins st7305
    PRIV_REQUIRES bt_ctl wifi_manager touch_panel audio_player gps sd_scan
                  svc_nvs nvs_flash driver esp_timer esp_hw_support
                  esp_driver_gpio esp_driver_usb_serial_jtag
)
