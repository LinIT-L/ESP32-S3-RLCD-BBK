/* lua_bridge.h — LinTOS Lua 桥总入口 (模块化)
 *
 * 分层: page_lua.c = 页面/状态机/加载/错误策略;
 *       lua_bridge/luat_lib_<模块>.c = 具体 app.* API (对标 LuatOS 的 luat_lib_*.c + luaopen_*)。
 * 加新 API: ① 对应模块文件写 static int l_xxx(lua_State *L)
 *           ② 该模块的 lua_mod_<x>_register() 里加一行 lua_setfield
 *           ③ 若是新模块, 在下面加声明 + 在 lua_bridge.c 的 k_mods[] 加一行
 *
 * ⚠ 共享状态**定义在 page_lua.c**(去掉了 static), 这里只 extern —— 搬家的函数引用名不用改。
 */
#pragma once
/* ---- 公共 include (与拆分前 page_lua.c 的那一套保持一致, 保证"搬家不改逻辑") ---- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "st7305.h"
#include "ui_common.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "os.h"
#include "os_pane.h"
#include "os_notify.h"
#include "input.h"
#include "board_battery.h"
#include "os_hid.h"
#include "os_wifi.h"
#include "os_net.h"
#include "wifi_manager.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"   /* app.net.fetch_*: HTTPS 走系统根证书包校验 */
#include "svc_nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "esp_netif_ip_addr.h"
#include "keyboard.h"
#include "netscan.h"

/* 桥各模块统一日志 tag (与 page_lua.c 保持一致) */
#ifndef TAG
#define TAG "LUAAPP"
#endif     /* netscan_port_probe / netscan_arp_dump 等 */   /* KBD_* / kbd_style_t / kbd_geom_default 等软键盘定义 */

/* ---- 共享状态 (定义在 page_lua.c) ---- */
extern lua_State       *s_L;
extern st7305_handle_t *s_lcd;
extern ui_ctx_t        *s_lua_ctx;
extern char             s_err[96];
extern bool             s_err_pending;
extern bool             s_exit_req;
/* ★ 全屏程序/游戏退出确认开关 (定义在 page_lua.c): 默认 true;
 *   脚本可 app.exit_confirm(false) 关掉 —— 见 docs/App制作规则.md「退出确认」。 */
extern bool             g_lua_exit_ask;

/* ---- 按键 FIFO (定义在 page_lua.c, 由 lua_action 投递) ---- */
#define KEY_FIFO_N 24
extern int  g_lua_keys[KEY_FIFO_N];
extern int  g_lua_keys_head, g_lua_keys_tail;
void        key_push(int a);

/* ---- 公共工具 (定义在 page_lua.c) ---- */
uint32_t pl_now_ms(void);
/* ★ 请求退出应用 (定义在 page_lua.c): app.exit() 与框架 BACK 共用的唯一入口。
 *   全屏程序/游戏默认先弹"退出程序?" — 见 docs/App制作规则.md「退出确认」。 */
void lua_request_exit(ui_ctx_t *ctx);


/* ---- 与 page_lua.c 共用的助手 (定义仍在 page_lua.c; 拆分时自动收集) ---- */
extern void app_big_text(st7305_handle_t *lcd, int x, int y, const char *s, int scale, bool inv);
extern void app_big_text_centered(st7305_handle_t *lcd, int cx, int y, const char *s, int scale, bool inv);
extern void kbd_lua_sync(void);
extern uint8_t lua_hidc(char c);
extern nvs_handle_t lua_nvs_h(void);
extern int os_core_db_del(const char *key);
extern int os_core_db_get(const char *key, char *out, size_t cap);
extern int os_core_db_set(const char *key, const char *val);
extern kbd_t s_kbd;
extern kbd_geom_t s_kbd_geom;
extern bool s_kbd_inited;
extern int utf8_len(const char *s);


/* ---- 跨模块共用的助手 (自动收集; 定义在原文件里, 已去 static) ---- */
extern void app_big_ascii(st7305_handle_t *lcd, int x, int y, char c, int scale, bool inv);
extern int l_wifi_clear_saved(lua_State *L);
extern uint32_t pl_now_ms(void);
extern bool s_tone_inited;

/* ---- 模块注册契约 ---- */
void lua_bridge_setup(lua_State *L);
void lua_mod_consts_register(lua_State *L);
void lua_mod_gfx_register(lua_State *L);
void lua_mod_input_register(lua_State *L);
void lua_mod_sys_register(lua_State *L);
void lua_mod_fs_register(lua_State *L);
void lua_mod_store_register(lua_State *L);
void lua_mod_net_register(lua_State *L);
void lua_mod_json_register(lua_State *L);
void lua_mod_wifi_register(lua_State *L);
void lua_mod_hid_register(lua_State *L);
void lua_mod_kbd_register(lua_State *L);
void lua_mod_img_register(lua_State *L);

/* ---- 几何参数取值器 (定义在 luat_lib_gfx.c, 非 static 供各模块共用) ----
 * 坐标/尺寸一律**容忍小数**(四舍五入); 颜色参数仍走严格的 luaL_checkinteger。
 * 见 luat_lib_gfx.c 里那段注释的成因 (Lua 5.3+ 对浮点整数的报错)。 */
int gfx_i(lua_State *L, int idx);
