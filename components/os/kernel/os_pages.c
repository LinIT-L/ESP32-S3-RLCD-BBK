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

/* ========== 内置应用表 (主菜单/应用管家共用) ==========
 * 顺序即主菜单图标顺序. icon_idx = 主菜单 SF 图标索引 (icons_main.inc).
 * id 全局唯一; 商店安装的 WASM 应用以 "wasm:<appId>" 运行时注册.
 * cat = 应用管家/商店 子分类: "游戏"/"程序"/"网络"/"运维"/"手册"/"学习", NULL=不归类. */
static const os_app_t s_builtin_apps[] = {
    { .id="bbk",     .label="\xe6\xad\xa5\xe6\xad\xa5\xe9\xab\x98", .icon_idx=21, .page=OS_PAGE_SELECT_GAME, .cat="\xe5\xad\xa6\xe4\xb9\xa0" }, /* 步步高 学习 (默认显示) */
    { .id="book",    .label="\xe9\x98\x85\xe8\xaf\xbb",             .icon_idx=9,  .page=OS_PAGE_BOOK,        .cat="\xe5\xad\xa6\xe4\xb9\xa0" }, /* 阅读 学习 */
    { .id="mp3",     .label="\xe9\x9f\xb3\xe4\xb9\x90",             .icon_idx=7,  .page=OS_PAGE_MP3_PLAYER,  .cat="\xe7\xa8\x8b\xe5\xba\x8f" }, /* 音乐 程序 */
    { .id="gamepad", .label="\xe6\x89\x8b\xe6\x9f\x84",             .icon_idx=3,  .page=OS_PAGE_GAMEPAD,     .cat="\xe8\xbf\x90\xe7\xbb\xb4" }, /* 手柄 运维 */
    { .id="wallpaper",.label="\xe5\xa3\x81\xe7\xba\xb8",            .icon_idx=10, .page=OS_PAGE_WALLPAPER,   .hidden=true }, /* 壁纸 (隐藏) */
    { .id="pomodoro",.label="\xe7\x95\xaa\xe8\x8c\x84\xe9\x92\x9f",.icon_idx=11, .page=OS_PAGE_POMODORO,    .hidden=true }, /* 番茄钟 (隐藏) */
    { .id="usb_hid", .label="\xe4\xbb\xbf\xe7\x9c\x9f\xe9\x94\xae\xe9\xbc\xa0", .icon_idx=1, .page=OS_PAGE_USB_HID, .hidden=true }, /* 仿真键鼠 (隐藏) */
    { .id="storage", .label="\xe5\xad\x98\xe5\x82\xa8",             .icon_idx=5,  .page=OS_PAGE_STORAGE,     .cat="\xe8\xbf\x90\xe7\xbb\xb4" }, /* 存储 运维 */
    { .id="settings",.label="\xe8\xae\xbe\xe7\xbd\xae",             .icon_idx=6,  .page=OS_PAGE_SETTINGS,    .cat="\xe8\xbf\x90\xe7\xbb\xb4" }, /* 设置 运维 */
    { .id="app_mgr", .label="\xe5\xba\x94\xe7\x94\xa8\xe7\xae\xa1\xe5\xae\xb6", .icon_idx=0, .page=OS_PAGE_APP_MANAGER, .hidden=true, .reveal=true }, /* 软件管家 (彩蛋揭示, 应用中心入口) */
    { .id="wb",      .label="\xe7\x99\xbd\xe6\x9d\xbf",             .icon_idx=18, .page=OS_PAGE_WHITEBOARD,  .hidden=true, .touch_only=true }, /* 白板 (隐藏, 触摸屏用) */
    { .id="term",    .label="\xe7\xbb\x88\xe7\xab\xaf",             .icon_idx=23, .page=OS_PAGE_TERMINAL,    .hidden=true }, /* 终端 (隐藏) */
    { .id="diag",    .label="\xe6\x95\x85\xe9\x9a\x9c\xe8\xaf\x8a\xe6\x96\xad", .icon_idx=22, .page=OS_PAGE_DIAGNOSIS, .hidden=true, .cat="\xe8\xbf\x90\xe7\xbb\xb4" }, /* 故障诊断 运维 */
    { .id="file",    .label="\xe6\x96\x87\xe4\xbb\xb6",             .icon_idx=19, .page=OS_PAGE_FILE_BROWSER,.hidden=true, .cat="\xe8\xbf\x90\xe7\xbb\xb4" }, /* 文件 运维 */
    { .id="touchtest",.label="\xe8\xa7\xa6\xe5\xb1\x8f\xe6\xb5\x8b\xe8\xaf\x95", .icon_idx=22, .page=OS_PAGE_TOUCHTEST, .hidden=true, .touch_only=true, .cat="\xe5\xb7\xa5\xe5\x85\xb7" }, /* 触屏测试 (隐藏, 触摸屏用) */
    /* WiFi键盘 无独立入口: 仅由 设置→无线连接→连接WiFi→选网 后预填 SSID、只输密码 */
    /* 隐藏的引擎 (可在应用管家/赞助页显示; 各自游戏选择菜单页) */
    { .id="gb",      .label="GB",    .icon_idx=8,  .page=OS_PAGE_GB_GAME,       .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* GB 游戏 */
    { .id="gbc",     .label="GBC",   .icon_idx=12, .page=OS_PAGE_GBC_GAME,      .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* GBC 游戏 */
    { .id="nes",     .label="NES",   .icon_idx=13, .page=OS_PAGE_NES_GAME,      .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* NES 游戏 */
    { .id="arduboy", .label="ArduBoy",.icon_idx=14,.page=OS_PAGE_ARDUBOY_GAME,  .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* ArduBoy 游戏 (os_pane 扫描 /sdcard/AB) */
    { .id="lavax",   .label="\xe6\x96\x87\xe6\x9b\xb2\xe6\x98\x9f", .icon_idx=16, .page=OS_PAGE_WQX,    .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* 文曲星 游戏 */
    { .id="vpet",    .label="\xe6\x9a\xb4\xe9\xbe\x99\xe6\x9c\xba", .icon_idx=15, .page=OS_PAGE_VPET,   .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* 暴龙机 游戏 */
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
    os_page_app_manager_register();   /* 应用管家 (应用管理+商店合并) */
    os_page_storage_register();       /* 存储管理 */
    os_page_engine_register_all();    /* GB/GBC/NES/文曲星/ArduBoy 游戏选择菜单页 */
    os_page_file_browser_register();  /* 文件浏览 */
    os_page_wifi_kb_register();       /* WiFi 虚拟键盘 */
    os_page_term_register();          /* 终端 */
    os_page_wb_register();            /* 白板 */
    os_page_diag_register();          /* 故障诊断 */
    os_page_sponsor_register();       /* 全屏弹窗 (第5类模板) */
    os_page_dialog_register();        /* 通用弹窗 (modal 覆盖页) */
    os_page_touchtest_register();     /* 多点触控测试工具 */

    /* 应用注册表 (主菜单/应用管理共用) */
    for (int i = 0; i < (int)BUILTIN_APP_COUNT; i++)
        os_app_register(&s_builtin_apps[i]);

    /* 全局服务 */
    os_svc_statusbar_register();
}
