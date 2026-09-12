# components/os — 标准化模块化 OS 内核
# 目录约定:
#   kernel/    页面栈/事件路由/输入分发/内存生命周期/页面装配
#   services/  全局服务 (状态栏/音量/电量/蓝牙等, 不占页面栈)
#   pages/     应用页模块 (每页一个 .c, 私有 static state)
idf_component_register(
    SRCS "kernel/os_core.c"
         "kernel/os_pages.c"
         "kernel/os_housekeeper.c"
         "kernel/os_dialog.c"
         "kernel/os_app.c"
         "kernel/os_pane.c"
         "kernel/os_theme.c"
         "kernel/os_hw.c"
         "kernel/os_mgr.c"
         "services/svc_statusbar.c"
         "services/svc_modtool.c"
         "pages/page_main.c"
         "pages/page_select_game.c"
         "pages/page_settings.c"
         "pages/page_gamepad.c"
         "pages/page_book.c"
         "pages/page_mp3.c"
         "pages/page_pomodoro.c"
         "pages/page_wallpaper.c"
         "pages/page_usb_hid.c"
         "pages/page_app_manager.c"
         "pages/page_storage.c"
         "pages/page_engine.c"
         "pages/page_term.c"
         "pages/page_wb.c"
         "pages/page_diag.c"
         "pages/page_sponsor.c"
         "pages/page_usb_bt.c"
        "pages/page_usb_net.c"
        "pages/page_winassist.c"
         "pages/page_keymanager.c"
         "pages/page_alarm.c"
         "pages/page_modtool.c"
         "pages/page_logdiag.c"
         "pages/page_calculator.c"
         "pages/page_wifiprobe.c"
         "pages/page_morse.c"
         "pages/page_netdect.c"
         "pages/page_fav.c"
    INCLUDE_DIRS "include"
    REQUIRES st7305 ui_common input touch_panel audio_player vibrator engine_manager gam4980 gb_emu gbc_emu nes_emu md_emu sms_emu lavax arduboy_avr vpet book_reader wallpapers usb_hid usb_bt usb_net wifi_manager fonts kbd_lib sd_scan board_battery esp_netif lwip spi_flash nvs_flash esp_system bt_ctl web_gamepad usbh_msc vpet virtual_keys board_shim keyvault calc wifi_probe macoui
        esp_http_client favorites esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-error=format-truncation
)
