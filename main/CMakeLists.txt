# Main 组件
# BBK 游戏模拟器 - 菜单系统前端

idf_component_register(
    SRCS "main.c" "remcon.c" "lua_launch.c"
    INCLUDE_DIRS "."
    REQUIRES
    hw_cfg board_pins
        self_test
        input
        app_board
        st7305
        bbk_boot
        os
        apps
        wallpapers
        driver
        esp_timer
        nvs_flash svc_time svc_log svc_nvs
        esp_event
        board_battery
        book_reader
        tone_player
        audio_player   # 2026-10-09: remcon 加 audio/vol/esr/esw 音频诊断命令
        gps            # 2026-10-09: remcon 加 gps 电源状态诊断 (耗电排查)
        espnow         # 2026-10-20: remcon 加 `now` ESP-NOW 诊断命令
        vibrator
        engine_manager
        board_shim
        sd_scan
        usb_bt
        usb_net
        espsdr         # espsdr_reclaim_boot: 频谱仪禁用则回收 64KB 采集区
        nes_emu
        spiffs
        lua
)
