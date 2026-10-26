/**
 * page_lua.c — Lua 单文件程序 通用应用页 (可放到内存卡运行的单文件程序).
 *
 * 页面结构: 单页双模式状态机.
 *   MODE_LIST — os_pane 双栏列表浏览 /sdcard/apps 下的 .lua 单文件; 确认选中 → 加载运行.
 *   MODE_RUN  — 为本次运行新建 lua_State (PSRAM 分配器), 执行该 .lua 单文件, 并把
 *               os 绘制/按键/时间/声音/震动回调桥接给 Lua (全局 table `app`).
 *
 * Lua 全局函数契约 (可选, 由脚本定义; 页面对应 os 回调里调用, 不存在则跳过):
 *   on_enter()  进入 RUN 态前调用
 *   render()    os 渲染回调里调用 (清屏 + 全套 app.* 自绘), 不要在里面 flush
 *   action(a)   os 按键回调里调用, 同时该 a 也投进 app.key() 的 FIFO
 *   poll()      os 每帧轮询里调用 (计时/刷新, 非阻塞)
 *   on_exit()   退出 RUN 态时调用 (清理/提示音收尾)
 *
 * 本页 fullscreen=true; os_render 会在模块 render 后统一 flush, 脚本绘制 API 不 flush.
 */
#include "os.h"
#include "netscan.h"   /* V1.2: 端口探测/ARP解析/MAC厂商 (L1 服务) */
#include "os_pane.h"
#include "lua_bridge.h"   /* 模块化后: 桥 API 在 components/apps/lua_bridge/ */
#include "gps.h"        /* 退出应用时兜底关 GPS (见 lua_stop) */
#include "gps_nmea.h"    /* gps_service_stop() */
#include "ui_common.h"
#include "os_notify.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"    /* EXT_RAM_BSS_ATTR: 大缓冲入 PSRAM, 省内部 RAM */
#include "esp_attr.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "input.h"
#include "esp_attr.h"      /* RTC_NOINIT_ATTR (崩溃定位探针) */
#include "esp_system.h"    /* esp_reset_reason() */
#include "esp_heap_caps.h" /* heap_caps_get_free_size (探针资源快照) */
#include "board_battery.h"
#include "os_hid.h"
#include "os_wifi.h"
#include "os_net.h"
#include "wifi_manager.h"
#include "esp_http_client.h"
#include "svc_nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include <dirent.h>
#include "esp_netif_ip_addr.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "keyboard.h"
#define TAG "LUAAPP"

#define LUA_ROOT_PATH "/sdcard/apps"
/* 应用管家接管启动: 待运行的 .lua 全路径; 非空则 on_enter 直接进 RUN, 一次性消费.
 * 独立 Lua 启动器已移除 (应用管家统一接管), 此路径由 os_page_lua_set_boot 设置. */
static char s_boot_path[128] = "";
void os_page_lua_set_boot(const char *path) {
    if (path) snprintf(s_boot_path, sizeof(s_boot_path), "%s", path);
    else      s_boot_path[0] = '\0';
}

/* ==== 崩溃定位探针 (2026-09-28 建立; 2026-09-29 增强资源快照) ====
 * 背景: 首次打开 Lua 应用整机重启, 串口在复位瞬间丢字抓不到 panic → 用阶段号反推死在哪一步.
 * 开机由 apps_register_all() 调 lua_probe_boot_report() 打印 (立即 + 10s 后复读一次).
 *
 * 2026-09-29 增强: 只记"哪一步"还不够 —— 加记**当时的资源快照**, 一次就能区分三类根因
 *   (本项目历史上三类都踩过):
 *     ① 栈被吃光   → "栈余" 接近 0 词 (main 任务默认栈 12288B, 实测稳态只剩 ~10.1KB 可借,
 *                     而 luaL_openlibs + 解析 30~45KB 脚本是 C 栈大户)
 *     ② 堆被吃光   → 内部堆/PSRAM 空闲接近 0
 *     ③ 禁 cache 窗口访问 PSRAM (lua_State 与脚本缓冲都在 PSRAM)
 *                  → 栈与堆都还够, 但复位原因=panic/assert (见项目约定"禁 cache 期间禁止 PSRAM 栈")
 *   另外记下**是哪个任务**跑到这一步: 若不在 main 上, 就不该是 main 的栈问题, 指向 ③。 */
#define LUA_PROBE_MAGIC 0x4C554150u     /* "LUAP" */
static RTC_NOINIT_ATTR uint32_t s_probe_magic;
static RTC_NOINIT_ATTR uint32_t s_probe_stage;
static RTC_NOINIT_ATTR char     s_probe_info[40];
static RTC_NOINIT_ATTR uint32_t s_probe_stack;   /* 当前任务栈历史最低余量 (词, 1 词=4B) */
static RTC_NOINIT_ATTR uint32_t s_probe_int;     /* 内部堆空闲 */
static RTC_NOINIT_ATTR uint32_t s_probe_ps;      /* PSRAM 空闲 */
static RTC_NOINIT_ATTR char     s_probe_task[16];

void lua_probe_stage(int stage, const char *info) {
    s_probe_magic = LUA_PROBE_MAGIC;
    s_probe_stage = (uint32_t)stage;
    s_probe_info[0] = '\0';
    if (info) snprintf(s_probe_info, sizeof(s_probe_info), "%s", info);
    /* 资源快照: 栈用"历史最低余量"(uxTaskGetStackHighWaterMark), 它是单调下降的,
     * 所以崩后再读到的就是**崩前最紧张那一刻**的值 ✓ */
    s_probe_stack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_probe_int   = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_probe_ps    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const char *tn = pcTaskGetName(NULL);
    snprintf(s_probe_task, sizeof(s_probe_task), "%s", tn ? tn : "?");
}

static void lua_probe_print_once(void) {
    esp_reset_reason_t r = esp_reset_reason();
    if (s_probe_magic == LUA_PROBE_MAGIC)
        ESP_LOGD("LUAPROBE", "上次复位前: 阶段=%u '%s' 任务=%s 栈余=%u词(≈%uB) 内部堆=%u PSRAM=%u | 复位原因=%d",
                 (unsigned)s_probe_stage, s_probe_info, s_probe_task,
                 (unsigned)s_probe_stack, (unsigned)s_probe_stack * 4u,
                 (unsigned)s_probe_int, (unsigned)s_probe_ps, (int)r);
    else
        ESP_LOGD("LUAPROBE", "无探针记录 | 本次复位原因=%d", (int)r);
}

/* 开机延迟复读: 复位瞬间 USB CDC 重枚举, 早期启动日志必丢 (实测 0.26s→5.1s 空档),
 * 开机 10 秒后再打一遍, 此时串口链路已稳定 → 崩溃现场必被抓到. */
static void lua_probe_delayed_cb(void *arg) {
    (void)arg;
    lua_probe_print_once();
}

void lua_probe_boot_report(void) {
    lua_probe_print_once();
    /* ⚠ 不在开机清零: 阶段号要一直保留到"下一次启动流程"覆盖它 ——
     * 否则复位后第一次上电就把崩溃现场抹掉, 再复位也读不到了.
     * 判读: 13 = 正常跑完; 1..12 = 崩在对应阶段; 0/'boot' = 还没启动过 Lua 应用. */
    const esp_timer_create_args_t targs = {
        .callback = lua_probe_delayed_cb, .name = "luaprobe" };
    esp_timer_handle_t th = NULL;
    if (esp_timer_create(&targs, &th) == ESP_OK)
        esp_timer_start_once(th, 10 * 1000 * 1000);
}

st7305_handle_t *s_lcd = NULL;
ui_ctx_t   *s_lua_ctx = NULL;   /* RUN 态可重绘上下文 */


typedef enum { LUA_MODE_LIST, LUA_MODE_RUN } lua_mode_t;

static lua_mode_t s_mode = LUA_MODE_LIST;
lua_State   *s_L = NULL;
char         s_err[96];      /* 最近一次 Lua 错误摘要 */
bool         s_err_pending;  /* 待在下一次 poll/action 弹 toast */
bool         s_exit_req;     /* 脚本调用 app.exit() 后置位, 页面择机退出 RUN 态 */
/* ★ 2026-10-03 全屏程序/游戏的"退出确认" (linit 规范: 按返回先问一句, 防误退丢进度)
 *   默认 true —— 所有 Lua 程序/游戏都弹; 脚本可 app.exit_confirm(false) 单独关掉。
 *   ⚠ 只在"脚本没消费 BACK"时生效: 脚本把 BACK 当返回键用 (关子窗/收菜单) 依旧优先。 */
bool         g_lua_exit_ask = true;
static bool  s_exit_dlg = false;   /* 确认框已弹出 → 防重复弹 (弹窗期间 BACK 归弹窗自己处理) */
static bool  s_exit_pend = false;  /* 已请求退出, 待下一帧 poll 统一弹确认框 (避免回调里重入) */
static bool  s_ran = false;        /* 应用是否真的跑起来过 (没跑起来 → 不弹确认, 直接退) */

/* ---- 按键 FIFO: 承接 os action, 供 Lua app.key() 非阻塞读取 ---- */
int g_lua_keys[KEY_FIFO_N];
int g_lua_keys_head, g_lua_keys_tail;   /* tail==head 时空 */

void key_push(int a) {
    int nt = (g_lua_keys_tail + 1) % KEY_FIFO_N;
    if (nt == g_lua_keys_head) {            /* 满 → 丢弃最旧一条 */
        g_lua_keys_head = (g_lua_keys_head + 1) % KEY_FIFO_N;
        g_lua_keys[nt] = a;
        g_lua_keys_tail = nt;
        return;
    }
    g_lua_keys[g_lua_keys_tail] = a;
    g_lua_keys_tail = nt;
}

/* ---- 双栏列表 (PSRAM 状态, 省内部 RAM) ---- */
/* ★ LINTOS 2026-10-25: 列表 pane(约 59KB) 原来静态常驻 PSRAM, 改为进页面才分配/退出即还 */
static os_pane_t *s_pane__p;
static os_pane_t *s_pane__get(void) {
    if (!s_pane__p) {
        s_pane__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pane__p) ESP_LOGW("PANE", "列表 pane 分配失败 (%u B)", (unsigned)sizeof(os_pane_t));
    }
    if (!s_pane__p) s_pane__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_8BIT);   /* 兜底: 内部 RAM */
    return s_pane__p;
}
static inline void s_pane__free(void) { if (s_pane__p) { heap_caps_free(s_pane__p); s_pane__p = NULL; } }
#define s_pane (*s_pane__get())

static bool is_lua(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".lua") == 0);
}

static void lua_stop(ui_ctx_t *ctx);

/* ---- 报错策略: 不再"一报错就退出应用" ----
 * 旧行为: poll()/render()/action() 任何报错都 lua_stop() → 用户感受是"应用一打开就没了",
 * 一个 typo 就白屏回列表, 连错在哪都看不到。
 * 新行为: 首次报错 → 串口打印 + 弹一次 toast, **应用继续跑**;
 *         但同一处**连续**报错到 LUA_ERR_MAX 次就退回列表(防死循环刷屏 / 防卡死)。
 * 任何一次成功调用即清零。 */
#define LUA_ERR_MAX 90
static int s_err_streak;

/* ---- 错误计数限流 + 屏幕常驻错误条 (2026-10-03 重做) ----
 * 旧实现的致命处: 计数**每帧都 +1**。而 render() 是每帧调用的 → 一个坏参数
 * (如给 bmpr 传了小数) 就每帧报一次 → LUA_ERR_MAX=90 只需 **3 秒**就攒满,
 * 应用被"打开三秒自动退出", 用户只看到一闪而过的 toast → 根因无从得知。
 * 现在:
 *   ① 计数**限流**: 两次计数至少隔 LUA_ERR_GAP_MS(500ms) → 90 次≈45 秒, 留足观察时间;
 *   ② **屏幕底部常驻错误条** (两行, 带 Lua 的行号与消息), 每次出错刷新, 持续 6 秒
 *      → 用户可以直接拍照/截图, 不用连串口;
 *   ③ toast 仍保留 (仅第一次), 保持原有观感。 */
#define LUA_ERR_GAP_MS     500
#define LUA_ERR_BANNER_MS  6000
static uint32_t s_err_last_ms;
static char     s_err_banner[136];
static uint32_t s_err_banner_until;

/* 按**字节**截断但同时避免切断 UTF-8 (错误消息多为 ASCII, 但应用的 msg 可能含中文) */
static void lua_err_clip(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void lua_report_error(ui_ctx_t *ctx, const char *name) {
    const char *m = lua_tostring(s_L, -1);
    uint32_t now = pl_now_ms();
    bool counted = (s_err_last_ms == 0) || (now - s_err_last_ms >= LUA_ERR_GAP_MS);
    if (counted) { s_err_streak++; s_err_last_ms = now; }
    snprintf(s_err_banner, sizeof(s_err_banner), "%s(): %s", name, m ? m : "?");
    s_err_banner_until = now + LUA_ERR_BANNER_MS;      /* 每次出错都续期 */
    if (s_err_streak <= 1) {
        ESP_LOGD(TAG, "Lua %s() 错误: %s", name, m ? m : "(null)");
        snprintf(s_err, sizeof(s_err), "%s: %s", name, m ? m : "?");
        s_err_pending = true;
    } else if ((s_err_streak % 30) == 0) {
        ESP_LOGD(TAG, "Lua %s() 连续出错 %d 次: %s", name, s_err_streak, m ? m : "(null)");
    }
    if (s_err_streak >= LUA_ERR_MAX) {
        ESP_LOGD(TAG, "Lua %s() 连续出错 %d 次 → 退回列表", name, s_err_streak);
        lua_stop(ctx);
    }
}

/* 屏幕底部两行错误条 (黑底白字, 48 字符/行 = 384px) */
static void lua_draw_err_banner(st7305_handle_t *lcd) {
    if (!lcd || !s_err_banner[0]) return;
    if ((int32_t)(s_err_banner_until - pl_now_ms()) <= 0) return;
    const int y0 = UI_SCREEN_H - 36;
    fill_rect(lcd, 0, y0, UI_SCREEN_W - 1, UI_SCREEN_H - 1, ST7305_COLOR_BLACK);
    char l1[49], l2[49];
    lua_err_clip(l1, sizeof(l1), s_err_banner);
    lua_err_clip(l2, sizeof(l2), s_err_banner + strlen(l1));
    draw_text(lcd, 4, y0 + 2,  l1, true);
    draw_text(lcd, 4, y0 + 19, l2, true);
}


/* ============ Lua 分配器: 全部走 PSRAM, 大块脚本/字符串不占内部 RAM ============ */
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) { if (ptr) heap_caps_free(ptr); return NULL; }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
}
void app_big_text(st7305_handle_t *lcd, int x, int y, const char *s, int scale, bool inv) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    for (int i = 0; s[i]; i++) app_big_ascii(lcd, x + i * 8 * scale, y, s[i], scale, inv);
}
void app_big_text_centered(st7305_handle_t *lcd, int cx, int y, const char *s, int scale, bool inv) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    int n = (int)strlen(s);
    int x = cx - n * 4 * scale;   /* 每字宽 8*scale, 居中 */
    app_big_text(lcd, x, y, s, scale, inv);
}

/* ============ Lua: 绘制桥 ============ */

/* UTF-8 首字节 → 该字符字节数。非法首字节/续字节一律按 1 处理, 保证步进永不越界
 * (旧代码写死 str += 3: 字符串里混入 ASCII 时会从半个汉字处继续解码 → 乱码,
 *  末尾 ASCII 还会越过字符串终止符多读 1~2 字节)。 */
int utf8_len(const char *s) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* ============ Lua: 软键盘 (kbd_lib 统一入口) ============
 * 项目里"很多键盘"的真相: 内核只有 kbd_lib 一份, 7 个页面共用, 但**外观有 4 种形态**
 * (std 全高 / edit 压低 / rows 只前几排 / dialog 缩放进弹窗)。历史上 draw 与 hit 是
 * 两组函数、要调用方自己配对, 一旦配错就命中全错且不报错。
 * 这里统一成"**一次 use(样式) 选定样式+几何+布局表, 之后 draw/hit 共用同一几何**":
 *
 *   app.kbd.use("std"[, y])                   5 排全高 145px, 布局 kbd_std
 *   app.kbd.use("edit"[, y])                  排高 19px(压低 1/3), 布局 kbd_edit
 *   app.kbd.use("rows", rows[, y])            只画前 rows 排, 布局 kbd_std
 *   app.kbd.use("dialog", x, y, w, row_h)     缩放进矩形, 布局 kbd_edit
 *   app.kbd.geom()  → {style=,x=,y=,w=,row_h=,rows=}
 *   app.kbd.height() → 当前样式占用像素高
 *   app.kbd.draw([press])                      用当前几何画
 *   app.kbd.hit(tx, ty)                        用同一几何命中 → 索引 / -1  (先 x 后 y)
 *   app.kbd.press(i) → 动作码(含 caps/shift/fn 副作用) | char(i) | label(i) | layer_on(i)
 *   app.kbd.get("caps"|"shift"|"fn") / set(...) / reset() / count()
 * 动作码常量与 KBD_ACT_* 同值: app.kbd.CHAR/ENTER/BKSP/SPACE/ESC/TAB/UP/DOWN/LEFT/RIGHT/CAPS/SHIFT/FN/CTRL/ALT
 * 样式常量: app.kbd.STYLE_STD/STYLE_EDIT/STYLE_ROWS/STYLE_DIALOG
 */
kbd_t     s_kbd;   /* [modsplit] 共享给 lua_bridge 各模块 */
kbd_geom_t s_kbd_geom;   /* [modsplit] 共享给 lua_bridge 各模块 */
bool      s_kbd_inited = false;   /* [modsplit] 共享给 lua_bridge 各模块 */

void kbd_lua_sync(void) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    if (!s_kbd_inited) {
        s_kbd = kbd_std;
        kbd_geom_default(&s_kbd_geom, KBD_STYLE_STD, ST7305_HEIGHT - KBD_H);
        s_kbd_inited = true;
    }
}

/* ---- app.nvs.* : 掉电保留键值 ---- */
nvs_handle_t lua_nvs_h(void) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    nvs_handle_t h;
    if (svc_nvs_open("luaapp", NVS_READWRITE, &h) != ESP_OK) return 0;
    return h;
}

/* ---- app.hid.* : 电脑键鼠 ---- */
/* ================= app.db — 系统核心互通数据库 (nvs 命名空间 core_db) ================= */
/* 存 金币/经验/玩家档案 等核心互通小KV; 键由调用方传 "ns:field" */
int os_core_db_get(const char *key, char *out, size_t cap){   /* [modsplit] 共享给 lua_bridge 各模块 */
    nvs_handle_t h;
    if(svc_nvs_open("core_db", NVS_READWRITE, &h)!=ESP_OK) return -1;
    size_t len=0;
    if(svc_nvs_get_str(h,key,NULL,&len)!=ESP_OK){ svc_nvs_close(h); return -1; }
    if(cap>0 && len>=cap){ svc_nvs_close(h); return -1; }
    esp_err_t e=svc_nvs_get_str(h,key,out,&len);
    svc_nvs_close(h);
    return e==ESP_OK?(int)len:-1;
}
int os_core_db_set(const char *key, const char *val){   /* [modsplit] 共享给 lua_bridge 各模块 */
    nvs_handle_t h;
    if(svc_nvs_open("core_db", NVS_READWRITE, &h)!=ESP_OK) return -1;
    esp_err_t e=svc_nvs_set_str(h,key,val);
    if(e==ESP_OK) e=svc_nvs_commit(h);
    svc_nvs_close(h);
    return e==ESP_OK?0:-1;
}
int os_core_db_del(const char *key){   /* [modsplit] 共享给 lua_bridge 各模块 */
    nvs_handle_t h;
    if(svc_nvs_open("core_db", NVS_READWRITE, &h)!=ESP_OK) return -1;
    esp_err_t e=svc_nvs_erase_key(h,key);
    if(e==ESP_OK) e=svc_nvs_commit(h);
    svc_nvs_close(h);
    return e==ESP_OK?0:-1;
}
uint8_t lua_hidc(char c) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x04 + (c - 'A'));
    if (c >= '0' && c <= '9') return (uint8_t)(0x1E + (c - '0'));
    switch (c) { case ' ': return 0x2C; case '.': return 0x37; case '-': return 0x2D; case ':': return 0x33; case '/': return 0x38; default: return 0; }
}

/* 注册全局 app 表 */

static void lua_setup_globals(lua_State *L) {
    /* 模块化: 具体 app.* API 都在 components/apps/lua_bridge/luat_lib_<模块>.c;
     * 这里只做调度(见 lua_bridge.h)。加新 API 去对应模块文件, 不要再往这里堆。 */
    lua_bridge_setup(L);
}


/* 读取 SD 卡脚本到 PSRAM 并执行; 成功返回 true */
static bool lua_load_run(const char *path) {
    lua_probe_stage(5, "load_run 入口");
    if (s_L) lua_close(s_L);
    s_L = lua_newstate(lua_psram_alloc, NULL, (unsigned)esp_timer_get_time());
    if (!s_L) { ESP_LOGD(TAG, "lua_newstate 失败"); return false; }
    lua_probe_stage(6, "newstate 成功");
    luaL_openlibs(s_L);
    lua_setup_globals(s_L);
    lua_probe_stage(7, "openlibs+bridge 成功");

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGD(TAG, "打开失败: %s", path);
        snprintf(s_err, sizeof(s_err), "\xe6\x97\xa0\xe6\xb3\x95\xe6\x89\x93\xe5\xbc\x80"); /* 无法打开 */
        s_err_pending = true; lua_close(s_L); s_L = NULL; return false; }
    lua_probe_stage(8, "fopen 成功");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); ESP_LOGD(TAG, "空文件: %s", path);
        snprintf(s_err, sizeof(s_err), "\xe7\xa9\xba\xe6\x96\x87\xe4\xbb\xb6"); /* 空文件 */
        s_err_pending = true; lua_close(s_L); s_L = NULL; return false; }
    char *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); ESP_LOGD(TAG, "PSRAM 分配 %ldB 失败", sz);
        snprintf(s_err, sizeof(s_err), "\xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3"); /* 内存不足 */
        s_err_pending = true; lua_close(s_L); s_L = NULL; return false; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    lua_probe_stage(9, "fread 完成");

    int rc = luaL_loadbuffer(s_L, buf, rd, "@app.lua");
    heap_caps_free(buf);
    lua_probe_stage(10, "loadbuffer 完成");
    if (rc != LUA_OK || lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
        const char *m = lua_tostring(s_L, -1);
        ESP_LOGD(TAG, "执行失败: %s", m ? m : "(null)");
        snprintf(s_err, sizeof(s_err), "%s", m ? m : "Lua error");
        s_err_pending = true;
        lua_close(s_L); s_L = NULL;
        return false;
    }
    lua_probe_stage(11, "pcall 完成 (脚本体跑完)");
    s_err_pending = false;
    s_err_streak = 0;               /* 新应用从干净计数开始 */
    return true;
}

/* ---- 调用 Lua 全局函数 (不存在则跳过) ----
 * arg = -1     无参调用
 * arg >= 0     先 push 一个整数参数再调用
 * Lua 要求"函数与参数在栈上连续、函数在下面", 所以必须**先 getglobal 再 push 参数**;
 * 旧代码只 getglobal 不 push 却传 nargs=1 → pcall 把函数下面的残留值当函数调用,
 * 表现为 action() 回调报错/收到垃圾值(所有依赖按键的 Lua 应用失效)。 */
static void lua_call_optional(ui_ctx_t *ctx, const char *name, int arg) {
    if (!s_L) return;
    lua_getglobal(s_L, name);
    if (!lua_isfunction(s_L, -1)) { lua_pop(s_L, 1); return; }
    int narg = 0;
    if (arg >= 0) { lua_pushinteger(s_L, (lua_Integer)arg); narg = 1; }
    if (lua_pcall(s_L, narg, 0, 0) != LUA_OK) {
        lua_report_error(ctx, name);   /* 容忍: 首次只 toast, 连续出错才回列表 */
        return;
    }
    s_err_streak = 0;
    s_err_last_ms = 0;
}

/* 调 Lua `action(a)` 并取回返回值: true = 脚本已消费该动作。
 * 用途: BACK 键先交给脚本(如关闭子窗口/保存后返回), 脚本 `return true` 才不退出应用;
 * 未定义 action 或未返回 true → 返回 false, 交回框架默认行为(退出)。对老脚本向后兼容。 */
static bool lua_call_action_consumed(ui_ctx_t *ctx, int a) {
    if (!s_L) return false;
    lua_getglobal(s_L, "action");
    if (!lua_isfunction(s_L, -1)) { lua_pop(s_L, 1); return false; }
    lua_pushinteger(s_L, (lua_Integer)a);
    if (lua_pcall(s_L, 1, 1, 0) != LUA_OK) {
        lua_report_error(ctx, "action");
        /* action() 挂了就不能把 BACK 吞掉: 明确退出应用(否则用户被卡在里面出不来) */
        if (s_mode == LUA_MODE_RUN) lua_stop(ctx);
        return false;
    }
    s_err_streak = 0;
    bool consumed = (lua_toboolean(s_L, -1) != 0);
    lua_pop(s_L, 1);
    return consumed;
}

/* ---- 退出 RUN 态: 调 on_exit + 释放 lua_State → 回 MODE_LIST ---- */
static void lua_stop(ui_ctx_t *ctx) {
    if (s_L) {
        lua_getglobal(s_L, "on_exit");
        if (lua_isfunction(s_L, -1)) {
            if (lua_pcall(s_L, 0, 0, 0) != LUA_OK)
                ESP_LOGD(TAG, "on_exit 错误: %s", lua_tostring(s_L, -1) ? lua_tostring(s_L, -1) : "(null)");
        }
        lua_close(s_L);
    }
    /* ★ 2026-10-24 GPS 硬闸门: 任何 Lua 应用退出(码表/地图/…), 一律停解析任务
     *   并把模块送回 UBX-RXM-PMREQ 待机(46µA)。脚本自己调 app.gps.stop() 是"应该做的",
     *   但它可能忘记 / 崩了 / 没写 on_exit ⇒ 在这里兜底, 否则退出后 GPS 仍在
     *   ~10mA 连续跟踪, 一夜就没电。两个函数都是幂等的, 多调无害。 */
    if (gps_is_ready()) {
        gps_service_stop();
        gps_release();   /* ★ 2026-10-24: 待机 + 卸 UART + 还原引脚, 不留任何常驻 */
    }
    s_L = NULL;
    s_mode = LUA_MODE_LIST;
    s_exit_req = false;
    s_exit_dlg = false;
    s_exit_pend = false;
    s_ran = false;
    g_lua_exit_ask = true;          /* 退出确认开关复位 → 下一个应用从"默认弹"开始 */
    s_err_streak = 0;               /* 换了应用/退出应用 → 报错计数归零 */
    os_notify_tone_stop();
    if (ctx) {
        lua_probe_stage(14, "lua_stop: pane reset 前");
        os_pane_reset(&s_pane);
        s_pane.focus = 1;
        os_pane_build_right(&s_pane);
        ctx->needs_redraw = true;
    }
}

/* ---- os_pane 右栏选中: 加载该 .lua 并进入 RUN 态 ---- */
static void lua_pane_select(ui_ctx_t *ctx, os_pane_t *p,
                            const char *path, const char *name) {
    (void)p; (void)name;
    if (!path || !path[0]) return;
    s_lcd = ctx->lcd;
    g_lua_keys_head = g_lua_keys_tail = 0;
    s_exit_req = false;
    ESP_LOGI(TAG, "启动: %s", path);
    if (lua_load_run(path)) {
        s_mode = LUA_MODE_RUN;
        lua_call_optional(ctx, "on_enter", -1);   /* 可选; 出错已内部回列表 */
        if (s_mode == LUA_MODE_RUN) ctx->needs_redraw = true;
    } else {
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
    }
}

/* ============ 页面回调 ============ */
static void lua_enter(ui_ctx_t *ctx) {
    lua_probe_stage(3, "lua_enter 进入");
    s_lcd = ctx->lcd;
    g_lua_keys_head = g_lua_keys_tail = 0;
    s_exit_req = false;
    s_exit_dlg = false;
    s_exit_pend = false;
    s_ran = false;
    g_lua_exit_ask = true;      /* 每个应用从默认值开始 (脚本可在 on_enter 里改) */
    /* ⚠ pane 身份必须在任何 early-return 之前初始化:
     * 应用管家直接拉起应用时走下面的 boot_path 分支直接 return, 不会经过"列表态"初始化块,
     * 此时 s_pane.root 仍为 NULL; 之后退出应用 → lua_stop() → os_pane_reset() → opendir(NULL)
     * 在 VFS 里空指针崩溃 (实测 LoadProhibited / EXCVADDR=0, 按 HOME 必崩)。 */
    if (!s_pane.loaded) {
        s_pane.root = LUA_ROOT_PATH;
        s_pane.is_item = is_lua;
        s_pane.on_select = lua_pane_select;
        s_pane.list_y = 20;
    }
    /* 应用管家接管: 若设置了启动脚本, 直接进 RUN 态运行; 否则才显示列表 */
    if (s_boot_path[0]) {
        char p[128];
        snprintf(p, sizeof(p), "%s", s_boot_path);
        s_boot_path[0] = '\0';                 /* 一次性消费 */
        lua_probe_stage(4, "boot_path 命中");
        if (lua_load_run(p)) {
            s_mode = LUA_MODE_RUN;
            lua_probe_stage(12, "RUN 态, 调 on_enter 前");
            lua_call_optional(ctx, "on_enter", -1);
            lua_probe_stage(13, "on_enter 返回");
            if (s_mode != LUA_MODE_RUN && s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
        } else if (s_err_pending) {
            s_err_pending = false;
            os_dialog_toast(ctx, s_err);
        }
        ctx->needs_redraw = true;
        return;
    }
    s_mode = LUA_MODE_LIST;
    os_pane_reset(&s_pane);      /* pane 身份已在函数开头保证, 此处只重扫目录 */
    if (!s_pane.root) s_pane.root = LUA_ROOT_PATH;   /* 双保险: 绝不让 root 为空进 opendir */
    s_pane.focus = 1;   /* 直接进右栏 */
    os_pane_build_right(&s_pane);
    ctx->needs_redraw = true;
}

static void lua_render(ui_ctx_t *ctx) {
    if (!ctx->lcd) return;
    s_lcd = ctx->lcd;
    if (s_mode == LUA_MODE_RUN) {
        lua_call_optional(ctx, "render", -1);   /* 出错已内部回列表 */
        lua_draw_err_banner(ctx->lcd);          /* ★ 出错时把原因画在屏上 (可截图), 不再只一闪 */
    } else {
        st7305_clear(ctx->lcd, ST7305_COLOR_WHITE);
        os_pane_render(ctx, &s_pane);
    }
}

/* ---- 退出确认回调 (全屏程序/游戏 按返回时弹的那一问) ----
 * result: 0=确定退出 / 1=取消 / -1=取消(BACK/超时)。
 * ⚠ 不能在弹窗回调里直接 os_pop 页面: 回调返回后 dlg_finish_top 才关弹窗层,
 *   此时栈顶仍是弹窗, 直接弹会把弹窗弹掉、应用留着。所以这里只置 s_exit_req,
 *   由下一帧 lua_poll 统一走"停 Lua + 弹栈"那条既有的退出路径 (与 app.exit() 同源)。 */
static void on_exit_confirm(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    s_exit_dlg = false;
    if (result == 0) {
        s_exit_req = true;
        if (ctx) ctx->needs_redraw = true;
    }
}

/* ★ 请求退出应用的**唯一入口** —— 脚本 app.exit() 与 框架 BACK 未消费 都走这里。
 *   规范: 全屏程序/游戏默认先弹"退出程序?"(确认才退, 取消留在原处), 防误触丢进度。
 *   弹窗动作**延迟到下一帧 poll** 执行 (s_exit_pend): 本函数会被 Lua 的 action()
 *   回调间接调用, 在回调里 push 弹窗属于重入 (项目已踩过"弹窗被替换"的坑)。
 *   ctx 为 NULL (极少见) 或应用没真正跑起来 (on_enter 就退) → 不弹, 直接退。 */
void lua_request_exit(ui_ctx_t *ctx) {
    if (!ctx) ctx = s_lua_ctx;
    if (g_lua_exit_ask && ctx && s_ran && s_mode == LUA_MODE_RUN) {
        s_exit_pend = true;
        ctx->needs_redraw = true;
        return;
    }
    s_exit_req = true;
}

static void lua_action(ui_ctx_t *ctx, os_action_t a) {
    s_lcd = ctx->lcd;
    if (s_mode == LUA_MODE_LIST) {
        if (os_pane_action(ctx, &s_pane, a)) return;
        if (a == OS_ACTION_BACK) os_pop(ctx);
        return;
    }
    /* RUN 态: 投进 FIFO (供 app.key() 轮询式取键, 向后兼容) + 把动作码当参数转发给 action(a) */
    key_push((int)a);
    if (s_mode == LUA_MODE_RUN) s_lua_ctx = ctx;
    /* BACK 先交给脚本: 脚本 `return true` = 自己处理了(关闭详情窗/保存后返回…), 不退出应用;
     * 未消费才由框架退出。旧实现在调用 action() **之前**就 os_pop → 脚本永远收不到 BACK,
     * 导致"返回前保存"(wb.lua)、"BACK 关详情窗"(wifiprobe.lua) 等全部失效。 */
    if (a == OS_ACTION_BACK) {
        if (lua_call_action_consumed(ctx, (int)a)) {
            if (ctx) ctx->needs_redraw = true;
            return;                                   /* 脚本已处理 → 留在应用内 */
        }
        if (s_mode != LUA_MODE_RUN) return;            /* 脚本出错已自行回列表 */
        /* ★ 全屏程序/游戏: 返回不再直接退出 —— 走统一入口, 默认先弹"退出程序?" */
        lua_request_exit(ctx);
        return;
    }
    lua_call_optional(ctx, "action", (int)a);   /* 按键值作为参数传给 Lua 的 action(a) */
    if (ctx) ctx->needs_redraw = true;      /* 按键后立即重绘, 否则 RUN 态不刷新 */
    if (s_mode != LUA_MODE_RUN) {
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
        return;
    }
    if (s_exit_req) {
        s_exit_req = false;
        lua_stop(ctx);
        /* ★ 2026-10-02 修「按返回退出 -> 白屏 + 一条竖直分割线, 再按一次才退出」:
         *   app.exit() 的语义是"退出应用", 必须把 Lua 页**弹出栈**。
         *   原来只 lua_stop() → 页面留在 LIST 态去扫 /sdcard/apps(空的) →
         *   用户看到的就是双栏列表的空壳(白屏 + 一条竖直分割线), 要再按一次 BACK 才真退出。
         *   循环弹 3 次: 兼顾"退出时上面还压着 toast/弹窗"的情况。 */
        for (int _i = 0; _i < 3 && ctx && ctx->current_page == OS_PAGE_LUA; _i++) os_pop(ctx);
    }
}

static bool lua_touch(ui_ctx_t *ctx, int x, int y) {
    /* RUN 态不消费触摸: 脚本自己在 poll()/render() 里调用 app.touch() 取坐标绘制,
     * 页面层不介入, 否则会与脚本的绘制/状态机抢同一次触摸事件。 */
    if (s_mode == LUA_MODE_LIST) return os_pane_touch(ctx, &s_pane, x, y);
    return false;
}

static void lua_poll(ui_ctx_t *ctx) {
    if (s_mode == LUA_MODE_LIST) { os_pane_poll(ctx, &s_pane); return; }
    /* ★ 退出确认: 由 lua_request_exit() 置位, 在这里(而非 action 回调里)才真正 push 弹窗。
     *   丢弃触发那一下残留的触摸 tap, 否则确认框一弹出就被当成"确定"自动退出
     *   (与 page_engine.c 的 eng_exit_confirm 同一套处理)。 */
    if (s_exit_pend) {
        s_exit_pend = false;
        s_exit_dlg = true;
        input_consume_tap(NULL, NULL);
        os_dialog_confirm_ex(ctx, "\xe9\x80\x80\xe5\x87\xba\xe7\xa8\x8b\xe5\xba\x8f?", /* 退出程序? */
                             0, -1, on_exit_confirm, NULL);
        return;
    }
    /* RUN 态: 先处理待办, 再转给 Lua poll() */
    if (s_exit_req) {
        s_exit_req = false;
        lua_stop(ctx);
        for (int _i = 0; _i < 3 && ctx && ctx->current_page == OS_PAGE_LUA; _i++) os_pop(ctx);
        return;
    }
    if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
    if (s_L) {
        lua_util_tick(s_L);            /* ★ 2026-10-24: 驱动 app.util 的 after/every 定时器 */
    }
    if (s_L) lua_call_optional(ctx, "poll", -1);
    if (s_mode == LUA_MODE_RUN) { s_lua_ctx = ctx; s_ran = true; }   /* 真跑起来了 → 退出才值得弹确认 */
    if (s_mode == LUA_MODE_RUN && ctx) ctx->needs_redraw = true;   /* 逐帧重绘: 支撑动画/触屏 */
    if (s_mode != LUA_MODE_RUN) s_lua_ctx = NULL;
    if (s_mode != LUA_MODE_RUN) {
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
    }
}

static void lua_on_exit(ui_ctx_t *ctx) {
    s_pane__free();   /* LINTOS: 页面退出归还列表 pane */
    /* 页面退栈 (BACK/HOME/回主菜单) 时兜底释放, 防止 lua_State 泄漏 */
    if (s_mode == LUA_MODE_RUN) lua_stop(ctx);
    s_lcd = NULL;
}

static const os_module_t s_mod_lua = {
    .name       = "lua",
    .page_id    = OS_PAGE_LUA,
    .on_enter   = lua_enter,
    .on_exit    = lua_on_exit,
    .render     = lua_render,
    .action     = lua_action,
    .touch      = lua_touch,
    .poll       = lua_poll,
    .fullscreen = true,
};

void os_page_lua_register(void) { os_register(&s_mod_lua); }
