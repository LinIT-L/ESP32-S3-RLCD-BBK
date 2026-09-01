/**
 * os_internal.h — OS 内部注册辅助（内核向页面装配层暴露的接口）.
 * 仅库内部使用, 不对外公开.
 */
#ifndef OS_INTERNAL_H
#define OS_INTERNAL_H

#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 由 os_pages.c 实现: 编译期注册所有内建模块(页面)到 os_register()。
 * os_init() 末尾调用, 让内核在启动时即可解析各页 id。 */
void os_register_all_internal(void);

/* ==== 页面模块注册函数 (components/os/pages 目录各文件实现) ====
 * 每页一个 .c, 私有 static state 留在各自文件; os_pages.c 统一装配. */
void os_page_main_register(void);          /* 主菜单桌面 */
void os_page_select_game_register(void);   /* 选择文曲星游戏 */
void os_page_settings_register(void);      /* 设置 */
void os_page_gamepad_register(void);       /* 手柄配置 */
void os_page_book_register(void);          /* 电子书 */
void os_page_mp3_register(void);           /* MP3 播放器 */
void os_page_pomodoro_register(void);      /* 番茄钟 */
void os_page_wallpaper_register(void);     /* 壁纸设置 */
void os_page_usb_hid_register(void);       /* USB HID 键鼠 */
void os_page_icon_editor_register(void);   /* 图标编辑器 */
void os_page_app_manager_register(void);   /* 应用管家 (应用管理+商店合并) */
void os_page_storage_register(void);       /* 存储管理 */
void os_page_engine_register_all(void);    /* GB/GBC/NES/文曲星/ArduBoy 游戏选择菜单页 */
void os_page_wifi_kb_register(void);       /* WiFi 虚拟键盘 */
void wifi_kb_preset_ssid(const char *ssid); /* 列表选网后预填 SSID, 只输密码 */
void os_page_term_register(void);          /* 终端 (P4) */
void os_page_wb_register(void);            /* 白板 (P4) */
void os_page_diag_register(void);          /* 故障诊断 (P4) */
void os_page_sponsor_register(void);       /* 全屏弹窗 (第5类模板) */
void os_page_usb_bt_register(void);        /* USB 蓝牙适配器 (USB-BT) */
void os_page_usb_net_register(void);       /* USB 网卡共享 (USB-RNDIS) */
void os_page_dialog_register(void);        /* 通用弹窗 (modal 覆盖页) */
void os_page_winassist_register(void);     /* 运维助手 (USB 复合) */
void os_page_keymanager_register(void);    /* 密钥管理器 (PIN+加密存储) */
void os_page_alarm_register(void);         /* 闹钟 */

#ifdef __cplusplus
}
#endif

#endif /* OS_INTERNAL_H */
