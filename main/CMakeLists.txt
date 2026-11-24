# Main 组件
# BBK 游戏模拟器 - 菜单系统前端

# 测试组件只在 LINTOS_LAVAX_BENCH=1 时挂上（默认不参与正式固件）
set(BENCH_REQ "")
if(LINTOS_LAVAX_BENCH)
    list(APPEND BENCH_REQ lavax_bench)
endif()

idf_component_register(
    SRCS "main.c" "remcon.c" "lua_launch.c" "min_boot.c"
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
        power_mgr
        driver
        esp_driver_gpio esp_driver_i2c
        esp_driver_tsens      # 最小系统: 片内温度传感器 (二分测发热)
        esp_timer
        nvs_flash svc_time svc_log svc_nvs
        svc_rtc        # 2026-11-20: 外挂 DS3231MZ+ 自动校时
        lpk
        esp_event
        board_battery
        book_reader
        tone_player
        audio_player   # 2026-10-09: remcon 加 audio/vol/esr/esw 音频诊断命令
        gps            # 2026-10-09: remcon 加 gps 电源状态诊断 (耗电排查)
        espnow         # 2026-10-20: remcon 加 `now` ESP-NOW 诊断命令
        vibrator
        backup         # 2026-11-22: remcon bkp 命令 + 各模块按需引用备份策略服务
        engine_manager
        board_shim
        sd_scan
        usb_bt
        usb_net
        espsdr         # espsdr_reclaim_boot: 频谱仪禁用则回收 64KB 采集区
        nes_emu
        spiffs
        lua
        ${BENCH_REQ}
    # 下面这些原本靠 apps 的公开 REQUIRES 传递拿到; apps 收窄为 PRIV 后必须自己声明。
    # (main 的源文件直接 include 这些组件的头, 只有 main 自己用 ⇒ PRIV)
    PRIV_REQUIRES
        gam4980        # main.c: gam4980_emu.h (开机自检/引擎状态)
        bt_ctl         # main.c: 蓝牙初始化
        touch_panel    # main.c: 触摸补探任务
        ui_common      # main.c: 字体/绘图
        usb_hid        # main.c: USB HID 模式
        wifi_manager   # main.c: 网络状态
        haptic         # main.c: 触觉效果库开机初始化 (注册'基础反馈→效果'钩子)
)

# CMake 变量不会自动变成宏 —— main.c 里的 #ifdef LINTOS_LAVAX_BENCH 需要显式定义
if(LINTOS_LAVAX_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LINTOS_LAVAX_BENCH=1)
endif()

# ★ 2026-11-22: 崩溃现场自留证 —— 把 esp_system 的 esp_panic_handler 换成
#   svc_crash.c 里的 __wrap_esp_panic_handler(先存现场到 RTC, 再走原流程)。
#   esp_system/port/panic_handler.c 是**跨编译单元**调用它, 所以链接期 --wrap 有效。
#   ⛔ 不能用 target_link_options(${COMPONENT_LIB} ...): main 被编成静态库,
#      CMake 对静态库目标设置链接选项**静默无效**, 实测构建通过但 wrap 根本没进 ELF
#      (nm 查不到 __wrap_esp_panic_handler, panic 现场照样丢)。
#   必须走 IDF 的链接属性, 它会落到最终的 .elf 链接命令行上。
idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=esp_panic_handler" APPEND)
