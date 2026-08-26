# components/os — 标准化模块化 OS 内核
# 目录约定:
#   kernel/    页面栈/事件路由/输入分发/内存生命周期/页面装配
#   services/  全局服务 (状态栏/音量/电量/蓝牙等, 不占页面栈)
#   pages/     应用页模块 (每页一个 .c, 私有 static state)
idf_component_register(
    SRCS "kernel/os_core.c"
         "kernel/os_pages.c"
         "kernel/os_dialog.c"
         "kernel/os_app.c"
         "services/svc_statusbar.c"
         "pages/page_main.c"
         "pages/page_select_game.c"
         "pages/page_settings.c"
         "pages/page_gamepad.c"
         "pages/page_book.c"
         "pages/page_mp3.c"
         "pages/page_pomodoro.c"
         "pages/page_wallpaper.c"
         "pages/page_usb_hid.c"
         "pages/page_icon_editor.c"
         "pages/page_app_manager.c"
         "pages/page_wifi_kb.c"
         "pages/page_term.c"
         "pages/page_wb.c"
         "pages/page_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES st7305 ui_common input audio_player vibrator engine_manager gam4980 book_reader wallpapers usb_hid
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
