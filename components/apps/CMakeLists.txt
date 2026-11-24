# components/apps — L4 应用层 (V1.2 从 os/pages 拆出)
# 目录约定:
#   pages/        每个应用页一个 .c (私有 static state), 含 am_* 应用中心子模块
#   apps_registry.c 内置应用装配 (页面注册 + 内置应用表)
#   include/apps.h  对外唯一入口 apps_register_all()
#
# 分层铁律: apps 只依赖 os 内核契约与 ui_common; 绝不反向被 os 依赖.
# 新增应用页: ①此处 SRCS 加一行 ②apps_registry.c 加一行 register ③应用表加一条.
idf_component_register(
    LDFRAGMENTS "linker.lf"   # ⚠ 关键: linker.lf 不会自动发现, 不在此登记则「静默失效」
    SRCS
        "apps_registry.c"
        "theme/r_carousel.c"
        "applock/applock.c"
        "pages/am_app_core.c"      # 2026-11-03: 统一应用管理核心(发现/身份/状态/动作 唯一来源)
        "pages/am_core_logic.c"    # 2026-11-03: 核心的纯逻辑部分(可宿主机单测, tests/app_core)
        "pages/lua_exit_policy.c"  # 2026-11-23: Lua 退出收尾判据(纯逻辑, 同上可单测)
        "pages/am_asset.c"
        "pages/am_asset_multi.c"   # 2026-10-31: 资源包多文件直接下载(替代 zip 解包)
        "pages/am_lpk.c"           # 2026-11-01: type=lpk 单文件资源包(下载即完成, 不解压)
        "pages/am_lpkg.c"          # 2026-11-01: 第三方 LPK 应用安装器(manifest/统一目录/注册表)
        "pages/am_manifest.c"      # 清单解析 + 路径守卫 (纯 C, 可主机单测)
        "pages/am_lua.c"
        "pages/am_store.c"
        "pages/am_store_json.c"
        "pages/am_store_state.c"   # 2026-11-01: 商店条目状态/版本比较 (纯 C, 可主机单测)
        "pages/am_store_net.c"
        "os_sources.c"
        "pages/page_app_manager.c"
        "pages/page_book.c"
        "pages/page_calculator.c"
        "pages/page_diag.c"
        "pages/diag_core.c"      # 纯逻辑: 归一化/排序/RC/事件ID (可主机测试)
        "pages/page_engine.c"
        "pages/page_fav.c"
        "pages/page_gamepad.c"
        "pages/page_keymanager.c"
        "pages/page_logdiag.c"
        "pages/page_lua.c"
        # ↓ Lua 桥模块 (2026-10-02 从 page_lua.c 拆出; 加新 API 去对应模块, 不要往 page_lua.c 堆)
        "lua_bridge/lua_bridge.c"
        "lua_bridge/lua_sandbox.c"
        "lua_bridge/lua_path_guard.c"
        "lua_bridge/lua_quota.c"
        "lua_bridge/luat_lib_consts.c"
        "lua_bridge/luat_lib_gfx.c"
        "lua_bridge/luat_lib_input.c"
        "lua_bridge/luat_lib_sys.c"
        "lua_bridge/luat_lib_fs.c"
        "lua_bridge/luat_lib_store.c"
        "lua_bridge/luat_lib_net.c"
        "lua_bridge/luat_lib_json.c"
        "lua_bridge/luat_lib_wifi.c"
        "lua_bridge/luat_lib_hid.c"
        "lua_bridge/luat_lib_kbd.c"
        "lua_bridge/luat_lib_img.c"
        "lua_bridge/luat_lib_pkg.c"
        "lua_bridge/luat_lib_gps.c"
        "lua_bridge/luat_lib_espnow.c"
        "lua_bridge/luat_lib_wiki.c"
        "lua_bridge/luat_lib_util.c"
        "lua_bridge/luat_lib_rec.c"
        "lua_bridge/luat_lib_haptic.c"   # 2026-11-24: app.haptic 触觉效果库
        "pages/page_main.c"
        "pages/page_modtool.c"
        "pages/page_morse.c"
        "pages/morse_codec.c"     # 摩斯码表/编解码/时间轴 (纯逻辑, 主机可测)
        "pages/morse_engine.c"    # 摩斯播放引擎 (自合成 PCM → os_notify_sound_feed)
        "pages/page_mp3.c"
        "pages/page_netdect.c"
        "pages/page_pomodoro.c"
        "pages/page_select_game.c"
        "pages/page_settings.c"
        "pages/page_sponsor.c"
        "pages/page_storage.c"
        "pages/page_term.c"
        "pages/page_usb_bt.c"
        "pages/page_usb_hid.c"
        "pages/page_usb_net.c"
        "pages/page_wallpaper.c"
        "pages/page_wb.c"
        "pages/page_wifiprobe.c"
        "pages/page_winassist.c"
        "pages/page_sdr.c"
        "pages/page_perfmon.c"   # 性能监视器 (2026-11-02: 从设置页独立出来的内置应用)
        "pages/page_pal.c"
        "pages/sdr_view.c"
    INCLUDE_DIRS "include" "pages" "lua_bridge"
    # ★ 2026-10-13: md_emu 默认停用(省 715KB), 恢复见 os.h 的 LINTOS_ENABLE_MD
    PRIV_REQUIRES apppack board_shim book_reader calc espnow favorites gam4980 gb_emu gbc_emu gps hw_cfg lavax lpk miniz nes_emu net_client sdlpal_port svc_rtc svc_time usb_bt usb_net usbh_msc vid_tune virtual_keys vpet wallpapers web_core web_files web_gamepad wiki arduboy_avr md_emu power_mgr sms_emu osutil
    REQUIRES
        os freertos netscan st7305 ui_common input touch_panel audio_player tone_player vibrator haptic engine_manager usb_hid wifi_manager fonts kbd_lib sd_scan board_battery esp_netif lwip spi_flash nvs_flash svc_cfg svc_nvs esp_system bt_ctl keyvault esp_http_client spiffs esp_timer wifi_manager lua espressif__esp-dsp mbedtls
    )

# 与 os 组件保持一致的告警策略 (页面代码原来在 os 组件下, 搬家后需显式继承)
target_compile_options(${COMPONENT_LIB} PRIVATE
    # ★ 2026-10-02 版本管理整合: 撤掉下面两条屏蔽 —— 它们曾让"定义了从未被调用"的静态函数长期不报警
    #   (全仓实测 25+ 个)。本项目自研代码要保持这层闸门; 第三方引擎目录另论。
    -Wno-error=format-truncation
)

# ============ 测试渠道源 (2026-11-08 linit) ============
# 需求: 新增应用先发「测试渠道」自测, 测过了再转正; 只有测试版固件连得上测试渠道。
# 做法: URL **只从环境变量读**, 源码/sdkconfig 里都不出现 (仓库是公开的, 不能带出去)。
#   LINTOS_TEST_SRC="https://bbk.linit.cn/p/<token>" idf.py build   → 测试固件
#   不给环境变量                                                    → 正式固件(二进制里没这个 URL)
# ⚠ 环境变量变化不会自动触发重编: 换源/去掉源都要先 `idf.py reconfigure`。
#   tools/store_channel.py build-fw 已经帮你处理了这一步。
if(DEFINED ENV{LINTOS_TEST_SRC} AND NOT "$ENV{LINTOS_TEST_SRC}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        LINTOS_TEST_SRC="$ENV{LINTOS_TEST_SRC}")
    message(STATUS "[apps] ★ 测试固件: 测试渠道源 = $ENV{LINTOS_TEST_SRC}")
else()
    message(STATUS "[apps] 正式固件: 无测试渠道源")
endif()
