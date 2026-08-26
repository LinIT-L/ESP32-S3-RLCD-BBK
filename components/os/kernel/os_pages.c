/**
 * os_pages.c — OS 页面装配层.
 *
 * 集中登记所有内建模块(页面)、全局服务与应用注册表。
 * 迁移规则: 每新增/迁移一个页面, 在此一行注册即可; 页面私有实现留在各自 pages 目录。
 * 页面模块只依赖 os 内核契约 (ui_ctx/os_module) 与 ui_common 绘制原语, 不依赖 menu。
 */
#include "os.h"
#include "os_internal.h"

/* ========== 全局服务 (components/os/services 目录) ========== */
/* 状态栏: services/svc_statusbar.c, 由 os_svc_statusbar_register 注册 */

/* ========== 内置应用表 (主菜单/应用管理共用) ==========
 * 顺序即主菜单图标顺序. icon_idx = 主菜单 SF 图标索引 (icons_main.inc).
 * id 全局唯一; 商店安装的 WASM 应用以 "wasm:<appId>" 运行时注册. */
static const os_app_t s_builtin_apps[] = {
    { "bbk",     "\xe6\xad\xa5\xe6\xad\xa5\xe9\xab\x98", 21, OS_PAGE_SELECT_GAME,  false }, /* 步步高 */
    { "book",    "\xe9\x98\x85\xe8\xaf\xbb",              9,  OS_PAGE_BOOK,         false }, /* 阅读 */
    { "mp3",     "\xe9\x9f\xb3\xe4\xb9\x90",              7,  OS_PAGE_MP3_PLAYER,   false }, /* 音乐 */
    { "gamepad", "\xe6\x89\x8b\xe6\x9f\x84",              3,  OS_PAGE_GAMEPAD,      false }, /* 手柄 */
    { "wallpaper","\xe5\xa3\x81\xe7\xba\xb8",            10,  OS_PAGE_WALLPAPER,    false }, /* 壁纸 */
    { "pomodoro","\xe7\x95\xaa\xe8\x8c\x84\xe9\x92\x9f", 11,  OS_PAGE_POMODORO,     false }, /* 番茄钟 */
    { "usb_hid", "\xe4\xbb\xbf\xe7\x9c\x9f\xe9\x94\xae\xe9\xbc\xa0", 1, OS_PAGE_USB_HID, false }, /* 仿真键鼠 */
    { "storage", "\xe5\xad\x98\xe5\x82\xa8",              5,  OS_PAGE_SETTINGS,     false }, /* 存储 */
    { "settings","\xe8\xae\xbe\xe7\xbd\xae",              6,  OS_PAGE_SETTINGS,     false }, /* 设置 */
    { "app_mgr", "\xe5\xba\x94\xe7\x94\xa8\xe7\xae\xa1\xe7\x90\x86", 0, OS_PAGE_APP_MANAGER, false }, /* 应用管理 */
    { "wb",      "\xe7\x99\xbd\xe6\x9d\xbf",             18,  OS_PAGE_WHITEBOARD,   false }, /* 白板 */
    { "term",    "\xe7\xbb\x88\xe7\xab\xaf",             23,  OS_PAGE_TERMINAL,     false }, /* 终端 */
};
#define BUILTIN_APP_COUNT (sizeof(s_builtin_apps) / sizeof(s_builtin_apps[0]))

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
    os_page_dialog_register();        /* 通用弹窗 (modal 覆盖页) */

    /* 应用注册表 (主菜单/应用管理共用) */
    for (int i = 0; i < (int)BUILTIN_APP_COUNT; i++)
        os_app_register(&s_builtin_apps[i]);

    /* 全局服务 */
    os_svc_statusbar_register();
}
