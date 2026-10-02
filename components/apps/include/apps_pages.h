#ifndef APPS_PAGES_H
#define APPS_PAGES_H

/* 内置应用页的"注册入口"声明 (V1.2 模块化: 从 os_internal.h 迁到 apps 层).
 * 分层理由: 页面入口点是**应用层内部**的装配细节, os 内核不该知道有哪些应用.
 * 新增应用: 由 tools/new_app.py 自动在此追加一行 + 在 apps_registry.c 调用.
 * 注: os_page_dialog_register 属 os 内核自带弹窗页, 仍在 os_internal.h 声明. */

void os_page_main_register(void);           /* 主菜单桌面 */
void os_page_select_game_register(void);    /* 选择文曲星游戏 */
void os_page_settings_register(void);       /* 设置 */
void os_page_gamepad_register(void);        /* 手柄配置 */
void os_page_book_register(void);           /* 电子书 */
void os_page_mp3_register(void);            /* MP3 播放器 */
void os_page_pomodoro_register(void);       /* 番茄钟 */
void os_page_wallpaper_register(void);      /* 壁纸设置 */
void os_page_usb_hid_register(void);        /* USB HID 键鼠 */
void os_page_app_manager_register(void);    /* 应用管家 (应用管理+商店合并) */
void os_page_storage_register(void);        /* 存储管理 */
void os_page_term_register(void);           /* 终端 (P4) */
void os_page_wb_register(void);             /* 白板 (P4) */
void os_page_diag_register(void);           /* 故障诊断 (P4) */
void os_page_sponsor_register(void);        /* 全屏弹窗 (第5类模板) */
void os_page_usb_bt_register(void);         /* USB 蓝牙适配器 (USB-BT) */
void os_page_usb_net_register(void);        /* USB 网卡共享 (USB-RNDIS) */
void os_page_winassist_register(void);      /* 运维助手 (USB 复合) */
void os_page_keymanager_register(void);     /* 密钥管理器 (PIN+加密存储) */
void os_page_modtool_register(void);        /* 修改机 (内存搜索工具) */
void os_page_logdiag_register(void);        /* 日志诊断 (Ghost-Audit 集成) */
void os_page_lvgl_register(void);           /* LVGL 演示页 (移植层自检) */
void os_page_calculator_register(void);     /* 计算器 */
void os_page_wifiprobe_register(void);      /* Wi-Fi 万用表 (扫描/热力图/探针) */
void os_page_morse_register(void);          /* 摩斯密码 (52键键盘+发声/震动) */
void os_page_netdect_register(void);        /* 网络分析 (主机发现/端口服务/弱口令/报告) */
void os_page_fav_register(void);            
void os_page_lua_register(void);            /* Lua single-file app */


void os_page_sdr_register(void);             /* 频谱仪 */

/* ★ 连接后自动映射 (C5): 让手柄页 on_enter 直接打开按键映射向导 (任何页面可触发) */
void page_gamepad_request_map_on_enter(void);

#endif /* APPS_PAGES_H */
