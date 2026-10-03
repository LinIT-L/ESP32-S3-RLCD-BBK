/**
 * apps_registry.c — 内置应用装配层 (V1.2 从 os/kernel/os_pages.c 拆出).
 *
 * 职责: ① 注册全部内置应用页模块 (apps/pages 下每个页面一个 .c)
 *       ② 登记内置应用表 s_builtin_apps[] (主菜单/应用中心共用)
 *
 * 分层: apps 是 L4 应用层, 只依赖 os 内核契约 (os.h/os_internal.h) 与 ui_common.
 *       反向依赖被禁止 —— os 不再 REQUIRE apps; 由 main 在 os_init() 后调 apps_register_all().
 * 迁移规则: 新增/迁移一个应用页, 只需 ①在此加一行 register ②在应用表加一条.
 */
#include "os.h"
#include "esp_log.h"          /* 启动自检日志 (ESP_LOGE/ESP_LOGI) */
#include "os_internal.h"      /* os 内核内部 (含 dialog 页注册) */
#include "apps_pages.h"        /* L4 应用页注册入口声明 (本层) */
#include "am_internal.h"       /* 崩溃定位探针 lua_probe_boot_report() */
#include "icons_main_enum.h"

/* ========== 内置应用表 (主菜单/应用管家共用) ==========
 * 顺序即主菜单图标顺序. icon_idx = main_icons[] 下标, 现为语义化 ICON_* 枚举 (icons_main_enum.h).
 * id 全局唯一; cat = 应用管家子分类, NULL=不归类.
 * cat = 应用管家/商店 子分类: "游戏"/"程序"/"网络"/"运维"/"手册"/"学习", NULL=不归类. */
static const os_app_t s_builtin_apps[] = {
    { .id="bbk",     .label="\xe6\xad\xa5\xe6\xad\xa5\xe9\xab\x98", .icon_idx=ICON_BBK, .page=OS_PAGE_SELECT_GAME, .cat="\xe5\xad\xa6\xe4\xb9\xa0" }, /* 步步高 学习 (默认显示) */
    { .id="book",    .label="\xe9\x98\x85\xe8\xaf\xbb\xe5\x99\xa8",             .icon_idx=ICON_BOOK,  .page=OS_PAGE_BOOK,        .cat="\xe5\xad\xa6\xe4\xb9\xa0" }, /* 阅读器 学习 */
    { .id="mp3",     .label="\xe9\x9f\xb3\xe4\xb9\x90",             .icon_idx=ICON_MP3,  .page=OS_PAGE_MP3_PLAYER,  .cat="\xe7\xa8\x8b\xe5\xba\x8f" }, /* 音乐 程序 */
    { .id="gamepad", .label="\xe6\x89\x8b\xe6\x9f\x84",             .icon_idx=ICON_GAMEPAD,  .page=OS_PAGE_GAMEPAD,     .cat="\xe7\xb3\xbb\xe7\xbb\x9f" }, /* 手柄 系统 */
    { .id="wallpaper",.label="\xe5\xa3\x81\xe7\xba\xb8",            .icon_idx=ICON_WALLPAPER, .page=OS_PAGE_WALLPAPER,   .hidden=true }, /* 壁纸 (隐藏) */
    { .id="usb_hid", .label="\xe4\xbb\xbf\xe7\x9c\x9f\xe9\x94\xae\xe9\xbc\xa0", .icon_idx=ICON_USB_HID, .page=OS_PAGE_USB_HID, .hidden=true, .cat="\xe8\xbf\x90\xe7\xbb\xb4" }, /* 仿真键鼠 运维 (隐藏) */
    { .id="storage", .label="\xe5\xad\x98\xe5\x82\xa8",             .icon_idx=ICON_STORAGE,  .page=OS_PAGE_STORAGE,     .cat="\xe7\xa8\x8b\xe5\xba\x8f" }, /* 存储 程序 */
    { .id="settings",.label="\xe8\xae\xbe\xe7\xbd\xae",             .icon_idx=ICON_SETTINGS,  .page=OS_PAGE_SETTINGS,    .cat="\xe7\xb3\xbb\xe7\xbb\x9f" }, /* 设置 系统 */
    { .id="app_mgr", .label="\xe5\xba\x94\xe7\x94\xa8\xe4\xb8\xad\xe5\xbf\x83", .icon_idx=ICON_APP_MGR, .page=OS_PAGE_APP_MANAGER, .hidden=true, .reveal=true }, /* 应用中心 (原"应用管家", 2026-09-20 改名) */
    /* ==== 内置工具应用: 2026-09-28 起**改以内置 C 版为准** ====
     * 背景: 2026-09-22 曾定"只留 Lua 版"(免烧录/可 TF 侧改), 但 Lua 版在真机上暴露问题
     * (例: 无线探测进入即整机重启 —— on_enter 里 app.wifi.init()+scan() 开射频, 触发掉电复位),
     * 且外置包占 apps 分区、与内置两份并存 (「应用→软件」两个"计算器")。
     *
     * linit 2026-09-28 定: **这 10 个全部改用内置 C 版**; 外置 Lua 版由他之后重新制作。
     * 配套动作 (已做):
     *   ① 打包好的 Lua 包已从 apps 分区移出 → backup_lua_apps_20260928/ (桌面源码未动)
     *   ② tools/sync_apps.py 的 MANIFEST 已注释 —— 再跑同步不会把 Lua 版塞回分区
     *   ③ 因此**商店不再显示这些应用** (商店只列 app 分区/TF 卡上扫到的 .lua 包)
     *
     * 显示名/子分类/图标沿用原 LAPP 头部 (sync_apps.py MANIFEST 的同一套), 用户习惯不变:
     *   app/ → 子分类"程序";  ops/ → 子分类"运维"。
     * ⚠ `.hidden=true`: **不自动进桌面(主菜单)**, 只在应用管家里可见 ——
     *   linit 规范 2026-09-28: 要上桌面由他自己在应用管家里长按添加 (os_app_set_mainmenu),
     *   不要替他往桌面塞图标。
     * ⚠ 要再切回 Lua 版: 把本节 10 条注释掉, 并放开 MANIFEST + 重跑 sync_apps.py。 */
    { .id="calc",      .label="计算器",   .icon_idx=ICON_CALC,      .page=OS_PAGE_CALC,      .hidden=true, .cat="程序" }, /* 计算器 程序 */
    { .id="morse",     .label="摩斯电码", .icon_idx=ICON_MORSE,     .page=OS_PAGE_MORSE,     .hidden=true, .cat="程序" }, /* 摩斯电码 程序 */
    { .id="pomodoro",  .label="番茄钟",   .icon_idx=ICON_POMODORO,  .page=OS_PAGE_POMODORO,  .hidden=true, .cat="程序" }, /* 番茄钟 程序 */
    { .id="wb",        .label="白板",     .icon_idx=ICON_WHITEBOARD,.page=OS_PAGE_WHITEBOARD,.hidden=true, .cat="程序" }, /* 白板 程序 */
    { .id="wifiprobe", .label="无线探测", .icon_idx=ICON_WIFIPROBE, .page=OS_PAGE_WIFIPROBE, .hidden=true, .cat="运维" }, /* 无线探测 运维 */
    { .id="netdect",   .label="网络检测", .icon_idx=ICON_NETDECT,   .page=OS_PAGE_NETDECT,   .hidden=true, .cat="运维" }, /* 网络检测 运维 */
    { .id="diag",      .label="电脑诊断", .icon_idx=ICON_DIAG,      .page=OS_PAGE_DIAGNOSIS, .hidden=true, .cat="运维" }, /* 电脑诊断 运维 */
    { .id="logdiag",   .label="日志诊断", .icon_idx=ICON_LOGDIAG,   .page=OS_PAGE_LOGDIAO,   .hidden=true, .cat="运维" }, /* 日志诊断 运维 */
    { .id="term",      .label="终端",     .icon_idx=ICON_TERMINAL,  .page=OS_PAGE_TERMINAL,  .hidden=true, .cat="运维" }, /* 终端 运维 */
    { .id="winassist", .label="运维助手", .icon_idx=ICON_WINASSIST, .page=OS_PAGE_WINASSIST, .hidden=true, .cat="运维" }, /* 运维助手 运维 */
    /* 文件浏览器不再作为独立应用: 已融合进 存储→浏览文件 (page_storage.c 内, 快扫/删除/进退) */
    { .id="usb_bt",  .label="USB\xe8\x93\x9d\xe7\x89\x99",      .icon_idx=ICON_USB_BT, .page=OS_PAGE_USB_BT, .hidden=true, .cat="\xe8\xbf\x90\xe7\xbb\xb4" }, /* USB蓝牙 运维 (默认隐藏, 从应用管家进) */
    { .id="usb_net", .label="USB\xe7\xbd\x91\xe5\x8d\xa1", .icon_idx=ICON_USB_NET, .page=OS_PAGE_USB_NET, .hidden=true, .cat="\xe8\xbf\x90\xe7\xbb\xb4" }, /* USB网卡共享 运维 (默认隐藏, 从应用管家) */
    /* WiFi键盘 无独立入口: 仅由 设置→无线连接→连接WiFi→选网 后预填 SSID、只输密码 */
    /* 隐藏的引擎 (可在应用管家/赞助页显示; 各自游戏选择菜单页) */
    { .id="gb",      .label="GB",    .icon_idx=ICON_GB,  .page=OS_PAGE_GB_GAME,       .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* GB 游戏 */
    { .id="gbc",     .label="GBC",   .icon_idx=ICON_GBC, .page=OS_PAGE_GBC_GAME,      .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* GBC 游戏 */
    { .id="nes",     .label="NES",   .icon_idx=ICON_NES, .page=OS_PAGE_NES_GAME,      .hidden=true, .reveal=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* NES 游戏 */
    { .id="md",      .label="MD",    .icon_idx=ICON_MD, .page=OS_PAGE_MD_GAME,       .hidden=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* MD/Genesis 游戏 (Gwenesis) */

    { .id="sms",     .label="SMS",   .icon_idx=ICON_SMS, .page=OS_PAGE_SMS_GAME,      .hidden=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* SMS/GG 游戏 (SMSPlus, 独立图标 index37) */

    { .id="arduboy", .label="ArduBoy",.icon_idx=ICON_ARDUBOY,.page=OS_PAGE_ARDUBOY_GAME,  .hidden=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* ArduBoy 游戏 (os_pane 扫描 /sdcard/AB) */

    { .id="lavax",   .label="\xe6\x96\x87\xe6\x9b\xb2\xe6\x98\x9f", .icon_idx=ICON_WQX, .page=OS_PAGE_WQX,    .hidden=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* 文曲星 游戏 */

    { .id="vpet",    .label="\xe6\x9a\xb4\xe9\xbe\x99\xe6\x9c\xba", .icon_idx=ICON_VPET, .page=OS_PAGE_VPET,   .hidden=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* 暴龙机 游戏 (暴龙机图标) */

    /* 修改机: 默认不进主菜单, 仅软件管家"独立游戏"分类可见 (icon_idx=ICON_MODTOOL).
     * 用户需求: 不默认显示主菜单; 不开机启动. */
    { .id="modtool", .label="\xe4\xbf\xae\xe6\x94\xb9\xe6\x9c\xba", .icon_idx=ICON_MODTOOL, .page=OS_PAGE_MODTOOL, .hidden=true, .cat="\xe6\xb8\xb8\xe6\x88\x8f" }, /* 修改机 游戏 (ICON_MODTOOL) */
    /* 网络检测/无线探测/日志诊断/运维助手/电脑诊断/终端 已在上方"内置工具应用"区统一登记 */
    /* 商店可下载应用 (默认隐藏, 从应用管家"商店"下载后解隐加入主菜单) */
    { .id="secure",   .label="密钥管理器", .icon_idx=ICON_SECURE, .page=OS_PAGE_PLACEHOLDER_SECURE, .hidden=true, .cat="程序" }, /* 密钥管理器 程序 */
    { .id="fav",   .label="收藏夹", .icon_idx=ICON_FAV, .page=OS_PAGE_FAV, .hidden=true, .cat="程序" }, /* 收藏夹 程序 (ICON_FAV) */
    { .id="sdr", .label="频谱仪", .icon_idx=ICON_WIFIPROBE, .page=OS_PAGE_SDR, .hidden=true, .cat="系统" }, /* 频谱仪 (new_app.py) */
};
#define BUILTIN_APP_COUNT (sizeof(s_builtin_apps) / sizeof(s_builtin_apps[0]))

/* ========== 装配 ========== */
void apps_register_all(void)
{
    /* 崩溃定位: 打印"上次复位前 Lua 启动走到哪一步 + 本次复位原因" (RTC 保留内存) */
    lua_probe_boot_report();
/* 应用页模块 (每页一个 .c) */
    os_page_main_register();          /* 主菜单桌面 */
    os_page_select_game_register();   /* 选择文曲星游戏 */
    os_page_settings_register();      /* 设置 */
    os_page_gamepad_register();       /* 手柄配置 */
    os_page_book_register();          /* 电子书 */
    os_page_mp3_register();           /* MP3 播放器 */
    os_page_wallpaper_register();     /* 壁纸设置 */
    os_page_usb_hid_register();       /* USB HID 键鼠 */
    os_page_app_manager_register();   /* 应用管家 (应用管理+商店合并) */
    os_page_storage_register();       /* 存储管理 */
    os_page_engine_register_all();    /* GB/GBC/NES/文曲星/ArduBoy 游戏选择菜单页 */
    os_page_sponsor_register();       /* 全屏弹窗 (第5类模板) */
    os_page_usb_bt_register();        /* USB 蓝牙适配器 (USB-BT) */
    os_page_usb_net_register();       /* USB 网卡共享 (USB-RNDIS) */
    os_page_dialog_register();        /* 通用弹窗 (modal 覆盖页) */
    os_page_modtool_register();       /* 修改机 (内存搜索工具) */
    /* ==== 以下 10 个内置工具页 (2026-09-29 恢复注册) ====
     * 历史: 2026-09-22 曾把这 10 个注册调用用 `#if 0` 关掉 (当时改以 Lua 单文件版为准,
     *       不注册可让 --gc-sections 回收整个 .obj, 省约 174KB flash)。
     * 2026-09-29: 应用表已把这 10 个 C 版登记回去 (Lua 包已移出 apps 分区), 因此这里
     *       **必须同步打开 `#if 1`** —— 否则症状是"应用管家里能看到、点了一点反应都没有"
     *       (os_push 因页面未注册而失败, 只在日志里留一句 "push: page N not registered")。
     * ⚠ 恢复后 flash 占用 +约 174KB (page_netdect 62.6K / page_wb 42.5K / page_wifeprobe 21.8K
     *   / page_term 19.3K / page_diag 9.6K / 其余 5 个共 14.3K), 当前 factory 区余量足够。
     * 另: 下方 `app_check_pages_registered()` 会在开机时报出"有应用但页面没注册"的错配。 */
#if 1
    os_page_term_register();          /* 终端 */
    os_page_wb_register();            /* 白板 */
    os_page_calculator_register();    /* 计算器 */
    os_page_diag_register();          /* 故障诊断 */
    os_page_logdiag_register();       /* 日志诊断 */
    os_page_netdect_register();       /* 网络分析 */
    os_page_wifiprobe_register();     /* Wi-Fi 万用表 */
    os_page_winassist_register();     /* 运维助手 */
    os_page_morse_register();         /* 摩斯密码 */
    os_page_pomodoro_register();      /* 番茄钟 */
#endif
    os_page_keymanager_register();    /* 密钥管理器 */
    os_page_fav_register();           /* 收藏夹 */
    os_page_lua_register();           /* Lua 单文件程序 */
    os_page_sdr_register();                /* 频谱仪 */

/* 应用注册表 (主菜单/应用管理共用) */
    for (int i = 0; i < (int)BUILTIN_APP_COUNT; i++)
        os_app_register(&s_builtin_apps[i]);

    /* ==== 开机自检: 应用表里的每个 app, 它的页面是否真的注册了? ====
     * 2026-09-29 教训: 10 个 C 版工具应用加回应用表后, 对应的 os_page_*_register()
     * 还留在 `#if 0` 里 → 应用管家里看得到、点了一点反应都没有, 且只在日志里留一句
     * "push: page N not registered"。这里开机就报出来, 一眼定位。 */
    {
        int bad = 0;
        for (int i = 0; i < (int)BUILTIN_APP_COUNT; i++) {
            const os_app_t *a = &s_builtin_apps[i];
            if (a->page < 0) continue;
            if (!os_page_registered((int)a->page)) {
                ESP_LOGE("OSAPP", "[自检] 应用 %s 的页面 %d 未注册 → 点它会打不开!",
                         a->id ? a->id : "?", (int)a->page);
                bad++;
            }
        }
        if (bad) ESP_LOGE("OSAPP", "[自检] 共 %d 个应用页面未注册, 请检查注册调用是否被条件编译关掉", bad);
        else     ESP_LOGI("OSAPP", "[自检] 内置应用页面注册完整 (%d 个)", (int)BUILTIN_APP_COUNT);
    }

    /* 注: 全局服务 (状态栏/修改机图标) 属 os 内核侧, 由 os_register_services() 注册,
     * 不在此处 —— 保证 apps 不反向依赖 os 内部服务实现. */
}
