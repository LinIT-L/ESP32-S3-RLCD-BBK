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
        "pages/am_lua.c"
        "pages/am_store.c"
        "pages/am_store_json.c"
        "pages/am_store_net.c"
        "pages/page_app_manager.c"
        "pages/page_book.c"
        "pages/page_calculator.c"
        "pages/page_diag.c"
        "pages/page_engine.c"
        "pages/page_fav.c"
        "pages/page_gamepad.c"
        "pages/page_keymanager.c"
        "pages/page_logdiag.c"
        "pages/page_lua.c"
        # ↓ Lua 桥模块 (2026-10-02 从 page_lua.c 拆出; 加新 API 去对应模块, 不要往 page_lua.c 堆)
        "lua_bridge/lua_bridge.c"
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
        "lua_bridge/luat_lib_gps.c"
        "pages/page_main.c"
        "pages/page_modtool.c"
        "pages/page_morse.c"
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
        "pages/sdr_view.c"
    INCLUDE_DIRS "include" "pages" "lua_bridge"
    REQUIRES os apppack netscan st7305 ui_common input touch_panel audio_player tone_player vibrator engine_manager gam4980 gb_emu gbc_emu nes_emu md_emu sms_emu lavax arduboy_avr vpet book_reader wallpapers usb_hid usb_bt usb_net wifi_manager fonts kbd_lib sd_scan board_battery esp_netif lwip spi_flash nvs_flash svc_cfg svc_nvs esp_system bt_ctl web_gamepad usbh_msc vpet virtual_keys board_shim keyvault calc esp_http_client favorites esp_timer lua espressif__esp-dsp lvgl_port gps
)

# 与 os 组件保持一致的告警策略 (页面代码原来在 os 组件下, 搬家后需显式继承)
target_compile_options(${COMPONENT_LIB} PRIVATE
    # ★ 2026-10-02 版本管理整合: 撤掉下面两条屏蔽 —— 它们曾让"定义了从未被调用"的静态函数长期不报警
    #   (全仓实测 25+ 个)。本项目自研代码要保持这层闸门; 第三方引擎目录另论。
    -Wno-error=format-truncation
)
