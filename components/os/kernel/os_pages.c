/**
 * os_pages.c — OS 页面装配层.
 *
 * 集中登记所有内建模块(页面)与全局服务。
 * 迁移规则: 每新增/迁移一个页面, 在此一行注册即可; 页面私有实现留在各自 pages 目录。
 * 页面模块只依赖 os 内核契约 (ui_ctx/os_module) 与 ui_common 绘制原语, 不依赖 menu。
 */
#include "os.h"
#include "os_internal.h"

/* ========== 全局服务 (components/os/services 目录) ========== */
/* 状态栏: services/svc_statusbar.c, 由 os_svc_statusbar_register 注册 */

/* ========== 装配 ========== */
void os_register_all_internal(void)
{
    /* 应用页模块 (每页一个 .c) */
    os_page_main_register();          /* 主菜单桌面 */
    os_page_select_game_register();   /* 选择文曲星游戏 */
    os_page_settings_register();      /* 设置 */
    os_page_gamepad_register();       /* 手柄配置 */
    os_page_book_register();          /* 电子书 */
    os_page_mp3_register();           /* MP3 播放器 */
    os_page_pomodoro_register();      /* 番茄钟 */
    os_page_wallpaper_register();     /* 壁纸设置 */
    os_page_usb_hid_register();       /* USB HID 键鼠 */
    os_page_icon_editor_register();   /* 图标编辑器 */
    os_page_app_manager_register();   /* 应用管理 */
    os_page_wifi_kb_register();       /* WiFi 虚拟键盘 */
    os_page_term_register();          /* 终端 */
    os_page_wb_register();            /* 白板 */
    os_page_diag_register();          /* 故障诊断 */

    /* 全局服务 */
    os_svc_statusbar_register();
}
