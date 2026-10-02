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
        ESP_LOGW("LUAPROBE", "上次复位前: 阶段=%u '%s' 任务=%s 栈余=%u词(≈%uB) 内部堆=%u PSRAM=%u | 复位原因=%d",
                 (unsigned)s_probe_stage, s_probe_info, s_probe_task,
                 (unsigned)s_probe_stack, (unsigned)s_probe_stack * 4u,
                 (unsigned)s_probe_int, (unsigned)s_probe_ps, (int)r);
    else
        ESP_LOGW("LUAPROBE", "无探针记录 | 本次复位原因=%d", (int)r);
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

static st7305_handle_t *s_lcd = NULL;
static ui_ctx_t   *s_lua_ctx = NULL;   /* RUN 态可重绘上下文 */

typedef enum { LUA_MODE_LIST, LUA_MODE_RUN } lua_mode_t;

static lua_mode_t s_mode = LUA_MODE_LIST;
static lua_State   *s_L = NULL;
static char         s_err[96];      /* 最近一次 Lua 错误摘要 */
static bool         s_err_pending;  /* 待在下一次 poll/action 弹 toast */
static bool         s_exit_req;     /* 脚本调用 app.exit() 后置位, 页面择机退出 RUN 态 */

/* ---- 按键 FIFO: 承接 os action, 供 Lua app.key() 非阻塞读取 ---- */
#define KEY_FIFO_N 24
static int s_keys[KEY_FIFO_N];
static int s_keys_head, s_keys_tail;   /* tail==head 时空 */

static void key_push(int a) {
    int nt = (s_keys_tail + 1) % KEY_FIFO_N;
    if (nt == s_keys_head) {            /* 满 → 丢弃最旧一条 */
        s_keys_head = (s_keys_head + 1) % KEY_FIFO_N;
        s_keys[nt] = a;
        s_keys_tail = nt;
        return;
    }
    s_keys[s_keys_tail] = a;
    s_keys_tail = nt;
}

/* ---- 双栏列表 (PSRAM 状态, 省内部 RAM) ---- */
static EXT_RAM_BSS_ATTR os_pane_t s_pane;

static bool is_lua(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".lua") == 0);
}

static void lua_stop(ui_ctx_t *ctx);

/* ============ Lua 分配器: 全部走 PSRAM, 大块脚本/字符串不占内部 RAM ============ */
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) { if (ptr) heap_caps_free(ptr); return NULL; }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
}

/* ============ 大号 ASCII (8×12 点阵整数倍放大, 仿 page_pomodoro) ============ */
static void app_big_ascii(st7305_handle_t *lcd, int x, int y, char c, int scale, bool inv) {
    int idx = (c >= 0x20 && c <= 0x7E) ? (c - 0x20) : ('?' - 0x20);
    st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
    st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
    fill_rect(lcd, x, y, x + 8 * scale - 1, y + 16 * scale - 1, bg);
    const uint8_t *bmp = FONT8X12[idx];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = bmp[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << (7 - col)))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    st7305_draw_pixel(lcd, x + col * scale + dx, y + row * scale + dy, fg);
        }
    }
}
static void app_big_text(st7305_handle_t *lcd, int x, int y, const char *s, int scale, bool inv) {
    for (int i = 0; s[i]; i++) app_big_ascii(lcd, x + i * 8 * scale, y, s[i], scale, inv);
}
static void app_big_text_centered(st7305_handle_t *lcd, int cx, int y, const char *s, int scale, bool inv) {
    int n = (int)strlen(s);
    int x = cx - n * 4 * scale;   /* 每字宽 8*scale, 居中 */
    app_big_text(lcd, x, y, s, scale, inv);
}

/* ============ Lua: 绘制桥 ============ */
static int lua_clear(lua_State *L) {
    st7305_color_t c = ST7305_COLOR_WHITE;
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) c = (st7305_color_t)((int)luaL_checkinteger(L, 1));
    st7305_clear(s_lcd, c);
    return 0;
}
static int lua_px(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 3));
    st7305_draw_pixel(s_lcd, x, y, c);
    return 0;
}
static int lua_fill(lua_State *L) {
    int x0 = (int)luaL_checkinteger(L, 1), y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3), y1 = (int)luaL_checkinteger(L, 4);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 5));
    fill_rect(s_lcd, x0, y0, x1, y1, c);
    return 0;
}
static int lua_rect(lua_State *L) {   /* app.store_rect / app.rect 同义 */
    int x0 = (int)luaL_checkinteger(L, 1), y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3), y1 = (int)luaL_checkinteger(L, 4);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 5));
    draw_rect_outline(s_lcd, x0, y0, x1, y1, c);
    return 0;
}
static int lua_hline(lua_State *L) {
    int x0 = (int)luaL_checkinteger(L, 1), x1 = (int)luaL_checkinteger(L, 2), y = (int)luaL_checkinteger(L, 3);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 4));
    draw_hline(s_lcd, x0, x1, y, c);
    return 0;
}
static int lua_vline(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y0 = (int)luaL_checkinteger(L, 2), y1 = (int)luaL_checkinteger(L, 3);
    st7305_color_t c = (st7305_color_t)((int)luaL_checkinteger(L, 4));
    draw_vline(s_lcd, x, y0, y1, c);
    return 0;
}
static int lua_text(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    draw_text(s_lcd, x, y, s, inv);
    return 0;
}
static int lua_textc(lua_State *L) {
    int y = (int)luaL_checkinteger(L, 1);
    const char *s = luaL_checkstring(L, 2);
    bool inv = (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? (lua_toboolean(L, 3) != 0) : false;
    draw_text_centered(s_lcd, y, s, inv);
    return 0;
}
static int lua_textw(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)text_width(s));
    return 1;
}
static int lua_big(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    int scale = (int)luaL_optinteger(L, 4, 1);
    bool inv = (lua_gettop(L) >= 5 && !lua_isnil(L, 5)) ? (lua_toboolean(L, 5) != 0) : false;
    app_big_text(s_lcd, x, y, s, scale, inv);
    return 0;
}
static int lua_bigc(lua_State *L) {
    int cx = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    int scale = (int)luaL_optinteger(L, 4, 1);
    bool inv = (lua_gettop(L) >= 5 && !lua_isnil(L, 5)) ? (lua_toboolean(L, 5) != 0) : false;
    app_big_text_centered(s_lcd, cx, y, s, scale, inv);
    return 0;
}

/* UTF-8 首字节 → 该字符字节数。非法首字节/续字节一律按 1 处理, 保证步进永不越界
 * (旧代码写死 str += 3: 字符串里混入 ASCII 时会从半个汉字处继续解码 → 乱码,
 *  末尾 ASCII 还会越过字符串终止符多读 1~2 字节)。 */
static int utf8_len(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* ---- app.zh(x,y,str[,inv]): 16×16 中文点阵, UTF-8 字符串逐字绘制 (照 page_diag dzh16) ---- */

/* ---- app.zh24(x,y,str[,inv]): 24x24 中文点阵 (固件 font_zh 字库, 每字 72 字节) ----
 * C 版 page_diag 的 dz22/dz22s 用的就是这个 (步进 24)。 */

/* ---- app.bmp(x, y, w, h, data): 直绘 1bpp 位图 (data = Lua 字符串, 每行 (w+7)/8 字节, MSB 在左, 1=黑) ----
 * 用途: 应用把图标数据内嵌在自己的 .lua 里 (见 LAPP), 一行调用画出来, 无需固件内置图标。 */
/* 大位图直绘 (**无大小上限**): 第 5 参是**二进制 Lua 串** (每字节 8 像素, MSB 在前, 1=黑,
 * 0=透明不画), 长度 >= ceil(w/8)*h。
 *
 * 为什么需要它: app.bmp 收 hex 串且有 2048B 上限 (约 128×128), 只够贴图标。白板/图片类
 * 应用要"整幅重绘" (载入 BMP 后的基线快照), 尺寸 336×268 = 11KB, 用 app.bmp 会被拒;
 * 逐像素 app.px 又要 ~9 万次跨界调用, 一次重绘就卡住。这里一次调用把整块贴上去。
 * 数据走 luaL_checklstring (长度感知, 二进制安全, 不做 UTF-8 转换)。 */
static int lua_bmpr(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3), h = (int)luaL_checkinteger(L, 4);
    size_t len = 0;
    const char *d = luaL_checklstring(L, 5, &len);
    if (w <= 0 || h <= 0) return 0;
    size_t need = (size_t)((w + 7) / 8) * (size_t)h;
    if (len < need) return 0;                 /* 长度不够 → 容错不画 */
    st7305_draw_bitmap_1bit(s_lcd, x, y, w, h, (const uint8_t *)d);
    return 0;
}

static int lua_bmp(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3), h = (int)luaL_checkinteger(L, 4);
    size_t len = 0;
    const char *hex = luaL_checklstring(L, 5, &len);
    if (w <= 0 || h <= 0) return 0;
    size_t need = (size_t)((w + 7) / 8) * (size_t)h;
    if (need > 2048 || len < need * 2) return 0;      /* 上限 2048B (约 128x128), 容错不画 */
    static uint8_t s_bmp[2048];                        /* 解码缓冲 (内部 BSS, 2KB) */
    static const char *HX = "0123456789abcdef";
    for (size_t i = 0; i < need; i++) {
        int hi = -1, lo = -1;
        char a = hex[i * 2], b = hex[i * 2 + 1];
        for (int k = 0; k < 16; k++) {
            if      (a == HX[k] || a == (char)(HX[k] - 32)) hi = k;
            if      (b == HX[k] || b == (char)(HX[k] - 32)) lo = k;
        }
        if (hi < 0 || lo < 0) return 0;
        s_bmp[i] = (uint8_t)((hi << 4) | lo);
    }
    st7305_draw_bitmap_1bit(s_lcd, x, y, w, h, s_bmp);
    return 0;
}
static int lua_zh24(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    while (*str) {
        int n = utf8_len(str);
        int idx = (n == 3) ? font_zh_find_utf8(str) : -1;
        st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
        st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
        fill_rect(s_lcd, x, y, x + ZH_FONT_W - 1, y + ZH_FONT_H - 1, bg);
        if (idx >= 0) {
            const uint8_t *bmp = zh_font_data[idx];
            for (int row = 0; row < ZH_FONT_H; row++)
                for (int col = 0; col < ZH_FONT_W; col++) {
                    int byte = bmp[row * 3 + (col / 8)];
                    st7305_draw_pixel(s_lcd, x + col, y + row, (byte & (1 << (7 - (col % 8)))) ? fg : bg);
                }
        }
        x += ZH_FONT_W;
        str += n;
    }
    return 0;
}
static int lua_zh16(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    while (*str) {
        int n = utf8_len(str);
        int idx = (n == 3) ? font_zh16_find_utf8(str) : -1;   /* 字库只收 3 字节汉字 */
        st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
        st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
        fill_rect(s_lcd, x, y, x + 15, y + 15, bg);
        if (idx >= 0) {
            const uint8_t *bmp = zh16_font_data[idx];
            for (int row = 0; row < 16; row++)
                for (int col = 0; col < 16; col++) {
                    int byte = bmp[row * 2 + (col / 8)];
                    st7305_draw_pixel(s_lcd, x + col, y + row, (byte & (1 << (7 - (col % 8)))) ? fg : bg);
                }
        }
        x += 16;
        str += n;
    }
    return 0;
}
/* ---- app.zhs(x,y,str[,inv]): 12×12 中文点阵 (23×23 字库 2×2 降采样, 照 page_diag dzh) ---- */
static int lua_zh(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    const int bpr = (ZH_FONT_W + 7) / 8;
    while (*str) {
        int n = utf8_len(str);
        int idx = (n == 3) ? font_zh_find_utf8(str) : -1;   /* 字库只收 3 字节汉字 */
        st7305_color_t bg = inv ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE;
        st7305_color_t fg = inv ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK;
        fill_rect(s_lcd, x, y, x + 11, y + 11, bg);
        if (idx >= 0) {
            const uint8_t *bmp = zh_font_data[idx];
            for (int r = 0; r < 12; r++)
                for (int c = 0; c < 12; c++) {
                    bool any = false;
                    for (int dy = 0; dy < 2 && !any; dy++)
                        for (int dx = 0; dx < 2 && !any; dx++) {
                            int row = r * 2 + dy, col = c * 2 + dx;
                            if (bmp[row * bpr + (col / 8)] & (1 << (7 - (col % 8)))) any = true;
                        }
                    st7305_draw_pixel(s_lcd, x + c, y + r, any ? fg : bg);
                }
        }
        x += 12;
        str += n;
    }
    return 0;
}

/* ============ Lua: 固件字模族 ============
 * 原则: **Lua 应用一律用固件字模, 绝不自绘字体**。
 * 下面这些 API 一一对应 ui_common.c 里的固件字体函数 (各 C 版应用页面用的就是它们),
 * 于是 Lua 版与 C 版 UI 同源同形:
 *   app.texts  8×12 ASCII   (draw_ascii_small)   — 小字/提示行
 *   app.textm  12×18 ASCII  (draw_ascii_medium)  — 中字
 *   app.textsb 8×16 ASCII   (draw_ascii_sb)      — 状态栏同款
 *   app.zhsm   12×12 中文   (draw_zh_small)      — 小中文
 *   app.zhsb   16×16 中文   (draw_zh_sb, 22/24 采样)
 *   app.mix    混合 16px    (ui_text16)          — 中文 font_zh16 + ASCII 8×12 垂直居中
 *   app.mixc   混合 16px 居中 (ui_text16c)
 *   app.mixw   混合 16px 行宽 (ui_text16w)
 * 所有绘制函数返回"结束 x"(下一字符起点), 脚本可接续排版。 */
static int lua_texts(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    for (size_t i = 0; s[i]; i++) draw_ascii_small(s_lcd, x + (int)i * 8, y, s[i], inv);
    lua_pushinteger(L, (lua_Integer)(x + (int)strlen(s) * 8));
    return 1;
}
static int lua_textm(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    for (size_t i = 0; s[i]; i++) draw_ascii_medium(s_lcd, x + (int)i * 12, y, s[i], inv);
    lua_pushinteger(L, (lua_Integer)(x + (int)strlen(s) * 12));
    return 1;
}
static int lua_textsb(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    for (size_t i = 0; s[i]; i++) draw_ascii_sb(s_lcd, x + (int)i * 8, y, s[i], inv);
    lua_pushinteger(L, (lua_Integer)(x + (int)strlen(s) * 8));
    return 1;
}
static int lua_zhsm(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    while (*str) { int n = utf8_len(str); draw_zh_small(s_lcd, x, y, str, inv); x += 12; str += n; }
    lua_pushinteger(L, (lua_Integer)x);
    return 1;
}
static int lua_zhsb(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    while (*str) { int n = utf8_len(str); draw_zh_sb(s_lcd, x, y, str, inv); x += 16; str += n; }
    lua_pushinteger(L, (lua_Integer)x);
    return 1;
}
static int lua_mix(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    ui_text16(s_lcd, x, y, s, inv);
    lua_pushinteger(L, (lua_Integer)(x + ui_text16w(s)));
    return 1;
}
static int lua_mixc(lua_State *L) {
    int cx = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    const char *s = luaL_checkstring(L, 3);
    bool inv = (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) ? (lua_toboolean(L, 4) != 0) : false;
    ui_text16c(s_lcd, cx, y, s, inv);
    lua_pushinteger(L, (lua_Integer)(cx - ui_text16w(s) / 2));
    return 1;
}
static int lua_mixw(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)ui_text16w(s));
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
static kbd_t     s_kbd;
static kbd_geom_t s_kbd_geom;
static bool      s_kbd_inited = false;

static void kbd_lua_sync(void) {
    if (!s_kbd_inited) {
        s_kbd = kbd_std;
        kbd_geom_default(&s_kbd_geom, KBD_STYLE_STD, ST7305_HEIGHT - KBD_H);
        s_kbd_inited = true;
    }
}

/* app.kbd.use(style[, ...]) — 一次选定 样式 + 几何 + 布局表 */
static int lua_kbd_use(lua_State *L) {
    const char *n = luaL_checkstring(L, 1);
    int st = kbd_style_by_name(n);
    if (st < 0) { lua_pushboolean(L, false); return 1; }

    if (st == KBD_STYLE_DIALOG) {
        int x  = (int)luaL_checkinteger(L, 2);
        int y  = (int)luaL_checkinteger(L, 3);
        int w  = (int)luaL_checkinteger(L, 4);
        int rh = (int)luaL_checkinteger(L, 5);
        s_kbd_geom.style = KBD_STYLE_DIALOG;
        s_kbd_geom.x = x; s_kbd_geom.y = y; s_kbd_geom.w = w;
        s_kbd_geom.row_h = rh; s_kbd_geom.rows = KBD_ROWS;
    } else if (st == KBD_STYLE_ROWS) {
        int rows = (int)luaL_optinteger(L, 2, KBD_ROWS);
        int y    = (int)luaL_optinteger(L, 3, ST7305_HEIGHT - KBD_ROW * rows);
        kbd_geom_default(&s_kbd_geom, KBD_STYLE_ROWS, y);
        s_kbd_geom.rows = rows;
    } else {
        int defy = (st == KBD_STYLE_EDIT) ? (ST7305_HEIGHT - KBD_EDIT_ROW * KBD_ROWS)
                                          : (ST7305_HEIGHT - KBD_H);
        int y = (int)luaL_optinteger(L, 2, defy);
        kbd_geom_default(&s_kbd_geom, (kbd_style_t)st, y);
    }
    /* 布局表随样式走: edit / dialog 用 kbd_edit, 其余用 kbd_std */
    s_kbd = (st == KBD_STYLE_EDIT || st == KBD_STYLE_DIALOG) ? kbd_edit : kbd_std;
    s_kbd_inited = true;
    lua_pushboolean(L, true);
    return 1;
}
static int lua_kbd_reset(lua_State *L) {
    (void)L;
    bool was_edit = s_kbd_inited && (s_kbd.keys == kbd_edit.keys);
    if (!s_kbd_inited) kbd_lua_sync();
    else s_kbd = was_edit ? kbd_edit : kbd_std;
    return 0;
}
static int lua_kbd_draw(lua_State *L) {
    kbd_lua_sync();
    int press = (int)luaL_optinteger(L, 1, -1);
    kbd_draw_style(s_lcd, &s_kbd_geom, &s_kbd, press);
    return 0;
}
static int lua_kbd_hit(lua_State *L) {          /* (tx, ty) → idx | -1 */
    kbd_lua_sync();
    int tx = (int)luaL_checkinteger(L, 1), ty = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, kbd_hit_style(&s_kbd, &s_kbd_geom, tx, ty));
    return 1;
}
static int lua_kbd_geom(lua_State *L) {
    kbd_lua_sync();
    lua_newtable(L);
    lua_pushstring(L, kbd_style_name(s_kbd_geom.style)); lua_setfield(L, -2, "style");
    lua_pushinteger(L, s_kbd_geom.x);      lua_setfield(L, -2, "x");
    lua_pushinteger(L, s_kbd_geom.y);      lua_setfield(L, -2, "y");
    lua_pushinteger(L, s_kbd_geom.w);      lua_setfield(L, -2, "w");
    lua_pushinteger(L, s_kbd_geom.row_h);  lua_setfield(L, -2, "row_h");
    lua_pushinteger(L, s_kbd_geom.rows);   lua_setfield(L, -2, "rows");
    return 1;
}
static int lua_kbd_height(lua_State *L) {
    kbd_lua_sync();
    lua_pushinteger(L, kbd_style_height(s_kbd_geom.style, s_kbd_geom.rows, s_kbd_geom.row_h));
    return 1;
}
static int lua_kbd_count(lua_State *L) { kbd_lua_sync(); lua_pushinteger(L, s_kbd.n); return 1; }
static int lua_kbd_press(lua_State *L) {
    kbd_lua_sync();
    lua_pushinteger(L, kbd_press(&s_kbd, (int)luaL_checkinteger(L, 1)));
    return 1;
}
static int lua_kbd_char(lua_State *L) {
    kbd_lua_sync();
    char c = kbd_char(&s_kbd, (int)luaL_checkinteger(L, 1));
    if (!c) { lua_pushnil(L); return 1; }
    char b[2] = { c, 0 };
    lua_pushstring(L, b);
    return 1;
}
static int lua_kbd_label(lua_State *L) {
    kbd_lua_sync();
    lua_pushstring(L, kbd_label(&s_kbd, (int)luaL_checkinteger(L, 1)));
    return 1;
}
static int lua_kbd_layer(lua_State *L) {
    kbd_lua_sync();
    lua_pushboolean(L, kbd_layer_on(&s_kbd, (int)luaL_checkinteger(L, 1)));
    return 1;
}
static int lua_kbd_get(lua_State *L) {          /* ("caps"|"shift"|"fn") */
    kbd_lua_sync();
    const char *k = luaL_optstring(L, 1, "caps");
    if (k && (k[0] == 's'))      lua_pushboolean(L, s_kbd.shift);
    else if (k && (k[0] == 'f')) lua_pushinteger(L, s_kbd.fn_mode);
    else                         lua_pushboolean(L, s_kbd.caps);
    return 1;
}
static int lua_kbd_set(lua_State *L) {          /* ("caps"|"shift"|"fn", v) */
    kbd_lua_sync();
    const char *k = luaL_checkstring(L, 1);
    if (k[0] == 's')      s_kbd.shift   = lua_toboolean(L, 2) != 0;
    else if (k[0] == 'f') s_kbd.fn_mode = (int)luaL_optinteger(L, 2, 0) % 3;
    else                  s_kbd.caps    = lua_toboolean(L, 2) != 0;
    return 0;
}

/* ============ Lua: 时间/输入/声音/震动/退出 ============ */
static int lua_millis(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}
static int lua_delay(lua_State *L) {
    int ms = (int)luaL_checkinteger(L, 1);
    /* 警告: 阻塞调用, 脚本勿在 render() 里用, 否则卡帧 */
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}
static int lua_key(lua_State *L) {
    if (s_keys_head != s_keys_tail) {
        int k = s_keys[s_keys_head];
        s_keys_head = (s_keys_head + 1) % KEY_FIFO_N;
        lua_pushinteger(L, (lua_Integer)k);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}
static bool s_tone_inited = false;
static int lua_tone(lua_State *L) {
    if (!s_tone_inited) { os_notify_tone_init(); s_tone_inited = true; }
    int f = (int)luaL_checkinteger(L, 1), ms = (int)luaL_checkinteger(L, 2);
    os_notify_tone((uint16_t)f, (uint16_t)ms);
    return 0;
}
static bool s_vib_inited = false;
static int lua_vibrate(lua_State *L) {
    if (!s_vib_inited) { os_notify_vibrate_init(); s_vib_inited = true; }
    int ms = (int)luaL_checkinteger(L, 1);
    int str = (int)luaL_optinteger(L, 2, 100);
    os_notify_vibrate_tap((uint16_t)ms, (uint8_t)str);
    return 0;
}
static int lua_exit(lua_State *L) {
    (void)L;
    s_exit_req = true;
    return 0;
}
/* app.redraw(): 标记本帧需重绘刷屏. Lua 应用 poll() 里逐帧刷新用(雷达/触屏绘制). */
static int lua_redraw(lua_State *L) {
    (void)L;
    ui_ctx_t *ctx = s_lua_ctx;
    if (ctx) ctx->needs_redraw = true;
    return 0;
}

/* 状态栏整条反白 (暗底页面用, 如无线探测的雷达视图)。
   注意: 脚本自己负责在 on_exit 里还原成 false, 否则会带到下一页。 */
static int lua_sbar_invert(lua_State *L) {
    bool v = lua_toboolean(L, 1);
    os_statusbar_set_invert(v);
    ui_ctx_t *ctx = s_lua_ctx;
    if (ctx) ctx->needs_redraw = true;
    return 0;
}

/* 本地墙上时钟 (与状态栏同源: time() + localtime), 返回 (时, 分, 秒)。
   给"按时间命名存档"这类需求用 (白板 wbHHMMSSSS.bmp 就是 C 版的做法)。
   系统时间未同步 (没联网/没设时间) 时返回的就是开机以来的错值, 脚本自行容忍。 */
static int lua_sys_clock(lua_State *L) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    lua_pushinteger(L, t ? t->tm_hour : 0);
    lua_pushinteger(L, t ? t->tm_min : 0);
    lua_pushinteger(L, t ? t->tm_sec : 0);
    return 3;
}

/* ============ Lua 桥: 触摸 / 系统 / 文件 / NVS / 网络 / WiFi / HID ============ */

/* ---- app.touch(): 返回 x,y 或 nil ---- */
static int lua_app_touch(lua_State *L) {
    int x, y;
    if (input_get_touch_pos(&x, &y)) {
        lua_pushinteger(L, (lua_Integer)x);
        lua_pushinteger(L, (lua_Integer)y);
        return 2;
    }
    return 0;
}
/* ---- app.touch_start(): 本次按下的起点 x,y (拖拽/判滑动方向用); 无手势返回 nil ----
 * 与 app.touch() 同帧使用: 起点固定、当前点变化, 两者相减即位移。 */
static int lua_touch_start(lua_State *L) {
    int x, y;
    if (input_touch_start_pos(&x, &y)) {
        lua_pushinteger(L, (lua_Integer)x);
        lua_pushinteger(L, (lua_Integer)y);
        return 2;
    }
    return 0;
}
/* ---- app.touch_hold(): 当前按住时长 ms (未按住返回 0); 脚本据此实现"长按" ---- */
static int lua_touch_hold(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)input_touch_hold_ms());
    return 1;
}

/* ---- app.sys.battery(): 返回电压 mV 或 nil ---- */
static int lua_sys_battery(lua_State *L) {
    board_battery_status_t st;
    if (board_battery_read_cached(&st) == ESP_OK) { lua_pushinteger(L, (lua_Integer)st.voltage_mv); return 1; }
    return 0;
}
static int lua_sys_heap(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return 1;
}
static int lua_sys_time(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

/* ---- app.fs.* : TF 卡文件存取 ---- */
static int lua_fs_exists(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    lua_pushboolean(L, access(p, F_OK) == 0);
    return 1;
}
/* app.fs.read 单次读取上限: 超过直接返回 nil, 避免一个大文件把堆吃光 */
#define LUA_FS_READ_MAX (512 * 1024)
static int lua_fs_read(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > LUA_FS_READ_MAX) { fclose(f); return 0; }
    /* 读缓冲优先走 PSRAM: 大脚本/素材不该占用宝贵的内部 RAM */
    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)sz + 1);   /* PSRAM 不足时退回内部 RAM */
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    lua_pushlstring(L, buf, rd);
    heap_caps_free(buf);
    return 1;
}
static int lua_fs_write(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    size_t len; const char *d = luaL_checklstring(L, 2, &len);
    FILE *f = fopen(p, "wb");
    if (!f) { lua_pushboolean(L, false); return 1; }
    bool ok = fwrite(d, 1, len, f) == len;
    fclose(f);
    lua_pushboolean(L, ok);
    return 1;
}
static int lua_fs_append(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    size_t len; const char *d = luaL_checklstring(L, 2, &len);
    FILE *f = fopen(p, "ab");
    if (!f) { lua_pushboolean(L, false); return 1; }
    bool ok = fwrite(d, 1, len, f) == len;
    fclose(f);
    lua_pushboolean(L, ok);
    return 1;
}
static int lua_fs_remove(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    lua_pushboolean(L, remove(p) == 0);
    return 1;
}
static int lua_fs_listdir(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    DIR *d = opendir(p);
    if (!d) return 0;
    lua_newtable(L);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        n++; lua_pushlstring(L, e->d_name, strlen(e->d_name));
        lua_rawseti(L, -2, n);
    }
    closedir(d);
    return 1;
}

/* ---- app.nvs.* : 掉电保留键值 ---- */
static nvs_handle_t lua_nvs_h(void) {
    nvs_handle_t h;
    if (svc_nvs_open("luaapp", NVS_READWRITE, &h) != ESP_OK) return 0;
    return h;
}
static int lua_nvs_get_int(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    nvs_handle_t h = lua_nvs_h();
    if (!h) return 0;
    int32_t v = 0;
    esp_err_t e = svc_nvs_get_i32(h, k, &v);
    svc_nvs_close(h);
    if (e != ESP_OK) return 0;
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}
static int lua_nvs_set_int(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    int32_t v = (int32_t)luaL_checkinteger(L, 2);
    nvs_handle_t h = lua_nvs_h();
    if (!h) { lua_pushboolean(L, false); return 1; }
    esp_err_t e = svc_nvs_set_i32(h, k, v);
    if (e == ESP_OK) svc_nvs_commit(h);
    svc_nvs_close(h);
    lua_pushboolean(L, e == ESP_OK);
    return 1;
}
static int lua_nvs_get_str(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    nvs_handle_t h = lua_nvs_h();
    if (!h) return 0;
    size_t len = 0;
    if (svc_nvs_get_str(h, k, NULL, &len) != ESP_OK) { svc_nvs_close(h); return 0; }
    char *s = malloc(len + 1);
    esp_err_t e = svc_nvs_get_str(h, k, s, &len);
    svc_nvs_close(h);
    if (e != ESP_OK) { free(s); return 0; }
    lua_pushstring(L, s);
    free(s);
    return 1;
}
static int lua_nvs_set_str(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    const char *v = luaL_checkstring(L, 2);
    nvs_handle_t h = lua_nvs_h();
    if (!h) { lua_pushboolean(L, false); return 1; }
    esp_err_t e = svc_nvs_set_str(h, k, v);
    if (e == ESP_OK) svc_nvs_commit(h);
    svc_nvs_close(h);
    lua_pushboolean(L, e == ESP_OK);
    return 1;
}

/* ---- app.net.* : 网络状态 / HTTP / ping ---- */
static int lua_net_is_connected(lua_State *L) {
    lua_pushboolean(L, wifi_manager_is_connected());
    return 1;
}
static int lua_net_get_ip(lua_State *L) {
    char ip[32];
    if (wifi_manager_get_ip(ip, sizeof(ip))) { lua_pushstring(L, ip); return 1; }
    return 0;
}
static int lua_net_http_get(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_optinteger(L, 2, 80);
    const char *path = luaL_optstring(L, 3, "/");
    int timeout = (int)luaL_optinteger(L, 4, 5000);
    char url[320];
    snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = timeout,
        .buffer_size = 2048, .disable_auto_redirect = false,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return 0;
    esp_err_t e = esp_http_client_perform(cl);
    if (e == ESP_OK) {
        /* ★ 2026-10-02: 4KB 缓冲搬 PSRAM —— 纯 socket 读(无 DMA/无禁 cache 窗口),
         *   留在内部 RAM 纯属浪费(本项目内部 RAM 最紧张, 见 docs/内部RAM预算…) */
        EXT_RAM_BSS_ATTR static char body[4097];
        int n = esp_http_client_read_response(cl, body, sizeof(body) - 1);
        if (n > 0) { body[n] = 0; lua_pushstring(L, body); esp_http_client_cleanup(cl); return 1; }
    }
    esp_http_client_cleanup(cl);
    return 0;
}
/* ---- app.net.ping(): 逐次 ICMP echo, 每次一个独立完整会话(count=1, 等 on_ping_end 才删会话) ---- */
typedef struct { volatile int done; volatile int ok; volatile uint32_t rtt; } lua_ping_ctx_t;
static lua_ping_ctx_t s_ping_ctx;
static void lua_ping_succ(esp_ping_handle_t h, void *arg) {
    (void)arg;
    uint32_t gap = 0;
    if (esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &gap, sizeof(gap)) == ESP_OK) s_ping_ctx.rtt = gap;
    s_ping_ctx.ok = 1;
}
static void lua_ping_end(esp_ping_handle_t h, void *arg) {
    (void)h; (void)arg; s_ping_ctx.done = 1;
}

/* ==== V1.2 新增: 局域网扫描 (netscan 服务) —— 解锁 netdect 类应用 ==== */
static int lua_net_port(lua_State *L) {          /* app.net.port(ip, port[, ms]) -> bool */
    const char *ip = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int ms   = (int)luaL_optinteger(L, 3, 300);
    lua_pushboolean(L, netscan_port_probe(ip, (uint16_t)port, ms) == 1);
    return 1;
}
static void lua_mac_str(const uint8_t m[6], char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static int lua_net_arp(lua_State *L) {           /* app.net.arp(ip) -> "AA:BB:.." 或 nil */
    const char *ip = luaL_checkstring(L, 1);
    uint8_t m[6];
    if (!netscan_resolve_mac(ip, m)) return 0;
    char s[18]; lua_mac_str(m, s);
    lua_pushstring(L, s);
    return 1;
}
static int lua_net_arp_list(lua_State *L) {      /* app.net.arp_list() -> {{ip=,mac=},...} */
    static char    ips[32][16];
    static uint8_t macs[32][6];
    int n = netscan_arp_dump(ips, macs, 32);
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        char s[18]; lua_mac_str(macs[i], s);
        lua_newtable(L);
        lua_pushstring(L, ips[i]); lua_setfield(L, -2, "ip");
        lua_pushstring(L, s);      lua_setfield(L, -2, "mac");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
/* ==== 本机网络信息 / MAC 厂商 (解锁 netdect: 网络 & 信息两页要的就是这一组) ==== */
/* 与 C 版 page_netdect 的 nd_get_local() 同源: esp_netif_get_ip_info + esp_netif_get_dns_info。
   返回 ip, 掩码, 网关, DNS, 接口名 (未联网 / 无 netif 时返回 0 个值)。 */
static int lua_net_ipinfo(lua_State *L) {
    esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!ni) ni = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ni) return 0;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ni, &ip) != ESP_OK) return 0;
    char b[20];
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.ip));      lua_pushstring(L, b);
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.netmask)); lua_pushstring(L, b);
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.gw));      lua_pushstring(L, b);
    b[0] = 0;
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(ni, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
        snprintf(b, sizeof(b), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    lua_pushstring(L, b);
    const char *d = esp_netif_get_desc(ni);
    lua_pushstring(L, d ? d : "?");
    /* 第 6 值: 本机 STA MAC (netdect 报告要"本机MAC") */
    uint8_t mac[6]; b[0] = 0;
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
        snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lua_pushstring(L, b);
    return 6;
}
/* MAC 文本 → OUI 厂商名; 未命中返回 nil (OUI 字典常驻固件, 与 netdect 的厂商显示同源) */
static int lua_net_vendor(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    unsigned m[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return 0;
    uint8_t mac[6] = { (uint8_t)m[0], (uint8_t)m[1], (uint8_t)m[2],
                       (uint8_t)m[3], (uint8_t)m[4], (uint8_t)m[5] };
    const char *v = NULL;
    if (os_net_mac_vendor(mac, &v) && v) { lua_pushstring(L, v); return 1; }
    return 0;
}
/* ==== 联网链 (解锁 netdect 的「选网 → 输密码 → 连接」) ==== */
static int lua_wifi_cur_ssid(lua_State *L) {     /* 已连接 SSID; 未连接返回 nil */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.ssid[0]) {
        lua_pushstring(L, (const char *)ap.ssid);
        return 1;
    }
    return 0;
}
static int lua_wifi_connecting(lua_State *L) { lua_pushboolean(L, wifi_manager_is_connecting()); return 1; }
static int lua_wifi_connect(lua_State *L) {      /* connect(ssid[, pass]) -> bool (保存并连接) */
    const char *ssid = luaL_checkstring(L, 1);
    const char *pass = luaL_optstring(L, 2, "");
    lua_pushboolean(L, wifi_manager_connect(ssid, pass));
    return 1;
}
static int lua_wifi_clear_saved(lua_State *L) { (void)L; wifi_manager_clear_saved(); return 0; }


static int lua_hid_cdc_read(lua_State *L) {      /* app.hid.cdc_read([maxlen]) -> string 或 nil */
    int want = (int)luaL_optinteger(L, 1, 256);
    if (want < 1) want = 1;
    if (want > 1024) want = 1024;
    char buf[1025];
    size_t n = os_hid_cdc_read((uint8_t *)buf, (size_t)want);
    if (n == 0) return 0;                        /* nil = 当前无数据 */
    lua_pushlstring(L, buf, n);
    return 1;
}
/* ==== 弹窗模块 app.dlg (让 Lua 应用的提示也走系统弹窗: 框架统一、只传文字) ====
 * 规范 (linit 2026-09-22): 所有弹窗提示必须走 os_dialog, 居中、0.5s 自动关闭,
 * 调用方只给文字 —— 框架/位置只有一处实现, 改框架全局生效。之前 Lua 无接口只能自绘,
 * 于是和系统弹窗长得不一样。
 * ⚠ 只做 fire-and-forget 的 toast; 需要"用户选完再回调"的 confirm/list 属于下一轮
 *   (要新增 on_dialog 契约回调, 改动面更大)。 */
static int lua_dlg_toast(lua_State *L) {          /* app.dlg.toast(msg[, ms]) */
    const char *msg = luaL_checkstring(L, 1);
    int ms = (int)luaL_optinteger(L, 2, 0);       /* 0 = 默认 0.5s */
    os_dialog_toast_ms(s_lua_ctx, msg, ms > 0 ? (uint32_t)ms : 0);
    return 0;
}

static int lua_net_ping(lua_State *L) {
    const char *ip = luaL_checkstring(L, 1);
    int count = (int)luaL_optinteger(L, 2, 4);
    int timeout = (int)luaL_optinteger(L, 3, 500);
    struct in_addr a;
    if (!inet_aton(ip, &a)) return 0;
    uint32_t niv = ntohl(a.s_addr);
    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        ip_addr_t t; IP_ADDR4(&t, (uint8_t)(niv>>24), (uint8_t)(niv>>16), (uint8_t)(niv>>8), (uint8_t)niv);
        esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
        cfg.target_addr = t; cfg.count = 1; cfg.interval_ms = 50;
        cfg.timeout_ms = (uint32_t)timeout; cfg.task_stack_size = 4096; cfg.task_prio = 4;
        esp_ping_callbacks_t cb = { .on_ping_success = lua_ping_succ, .on_ping_end = lua_ping_end, .cb_args = &s_ping_ctx };
        esp_ping_handle_t h = NULL;
        if (esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) { lua_pushboolean(L, false); lua_rawseti(L, -2, i*2+1); lua_pushinteger(L, 0); lua_rawseti(L, -2, i*2+2); continue; }
        s_ping_ctx.done = 0; s_ping_ctx.ok = 0; s_ping_ctx.rtt = 0;
        esp_ping_start(h);
        uint32_t t0 = (uint32_t)(esp_timer_get_time()/1000);
        while (!s_ping_ctx.done && (uint32_t)(esp_timer_get_time()/1000) - t0 < (uint32_t)timeout + 100) vTaskDelay(4);
        esp_ping_delete_session(h);
        vTaskDelay(pdMS_TO_TICKS(10));
        lua_pushboolean(L, (int)s_ping_ctx.ok);
        lua_rawseti(L, -2, i*2+1);
        lua_pushinteger(L, (lua_Integer)s_ping_ctx.rtt);
        lua_rawseti(L, -2, i*2+2);
    }
    return 1;
}


/* ---- app.wifi.* : 无线探测 ---- */
static int lua_wifi_init(lua_State *L) {
    lua_pushboolean(L, os_wifi_probe_init() == ESP_OK);
    return 1;
}
static int lua_wifi_scan(lua_State *L) {
    lua_pushboolean(L, os_wifi_scan() == ESP_OK);
    return 1;
}
static int lua_wifi_scan_done(lua_State *L) {
    lua_pushboolean(L, os_wifi_is_scan_done());
    return 1;
}
static int lua_wifi_ap_count(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)os_wifi_ap_count());
    return 1;
}
static int lua_wifi_ap_info(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    const wifi_ap_record_t *ap = os_wifi_ap_record(i);
    if (!ap) return 0;
    lua_pushinteger(L, (lua_Integer)ap->rssi);
    lua_pushstring(L, (const char *)ap->ssid);
    lua_pushinteger(L, (lua_Integer)ap->primary);      /* 信道 */
    lua_pushinteger(L, (lua_Integer)ap->authmode);     /* 加密 */
    char mac[18]; snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
        ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    lua_pushstring(L, mac);
    return 5;
}
static int lua_wifi_probe_init(lua_State *L) {
    lua_pushboolean(L, os_wifi_sniff_start() == ESP_OK);
    return 1;
}
static int lua_wifi_probe_stop(lua_State *L) {
    os_wifi_sniff_stop();
    return 0;
}
static int lua_wifi_probe_count(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)os_wifi_probe_count());
    return 1;
}
static int lua_wifi_probe_info(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    const wp_probe_t *p = os_wifi_probe_get(i);
    if (!p) return 0;
    char mac[18]; snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
        p->sa[0], p->sa[1], p->sa[2], p->sa[3], p->sa[4], p->sa[5]);
    lua_pushstring(L, mac);
    lua_pushinteger(L, (lua_Integer)p->rssi);
    lua_pushinteger(L, (lua_Integer)p->channel);
    /* V1.3: 补 ssid 与捕获次数 —— C 版探针页要显示 "ssid" 与 "x<count> CH<n>" */
    lua_pushstring(L, p->ssid);
    lua_pushinteger(L, (lua_Integer)p->count);
    return 5;
}
static int lua_wifi_release(lua_State *L) {
    os_wifi_probe_deinit();
    return 0;
}
static int lua_wifi_ch_stat(lua_State *L) {
    const wp_ch_stat_t *ch = os_wifi_ch_stat();
    if (!ch) return 0;
    lua_newtable(L);
    for (int i=0;i<WP_CH_COUNT;i++){
        lua_pushinteger(L, (lua_Integer)ch[i].beacon);
        lua_pushinteger(L, (lua_Integer)ch[i].probe);
        lua_createtable(L,0,2);
        lua_pushvalue(L,-3); lua_setfield(L,-2,"beacon");
        lua_pushvalue(L,-3); lua_setfield(L,-2,"probe");
        lua_rawseti(L, -4, i+1);
        lua_pop(L, 2);
    }
    return 1;
}

/* ---- app.hid.* : 电脑键鼠 ---- */
/* ================= app.db — 系统核心互通数据库 (nvs 命名空间 core_db) ================= */
/* 存 金币/经验/玩家档案 等核心互通小KV; 键由调用方传 "ns:field" */
static int os_core_db_get(const char *key, char *out, size_t cap){
    nvs_handle_t h;
    if(svc_nvs_open("core_db", NVS_READWRITE, &h)!=ESP_OK) return -1;
    size_t len=0;
    if(svc_nvs_get_str(h,key,NULL,&len)!=ESP_OK){ svc_nvs_close(h); return -1; }
    if(cap>0 && len>=cap){ svc_nvs_close(h); return -1; }
    esp_err_t e=svc_nvs_get_str(h,key,out,&len);
    svc_nvs_close(h);
    return e==ESP_OK?(int)len:-1;
}
static int os_core_db_set(const char *key, const char *val){
    nvs_handle_t h;
    if(svc_nvs_open("core_db", NVS_READWRITE, &h)!=ESP_OK) return -1;
    esp_err_t e=svc_nvs_set_str(h,key,val);
    if(e==ESP_OK) e=svc_nvs_commit(h);
    svc_nvs_close(h);
    return e==ESP_OK?0:-1;
}
static int os_core_db_del(const char *key){
    nvs_handle_t h;
    if(svc_nvs_open("core_db", NVS_READWRITE, &h)!=ESP_OK) return -1;
    esp_err_t e=svc_nvs_erase_key(h,key);
    if(e==ESP_OK) e=svc_nvs_commit(h);
    svc_nvs_close(h);
    return e==ESP_OK?0:-1;
}

static int lua_db_set(lua_State *L){
    const char *k=luaL_checkstring(L,1); const char *v=luaL_checkstring(L,2);
    lua_pushboolean(L, os_core_db_set(k,v)==0); return 1;
}
static int lua_db_get(lua_State *L){
    const char *k=luaL_checkstring(L,1); char b[128];
    if(os_core_db_get(k,b,sizeof(b))<0){ lua_pushnil(L); return 1; }
    lua_pushstring(L,b); return 1;
}
static int lua_db_del(lua_State *L){
    const char *k=luaL_checkstring(L,1); lua_pushboolean(L, os_core_db_del(k)==0); return 1;
}
static int lua_db_seti(lua_State *L){
    const char *k=luaL_checkstring(L,1); char b[32]; snprintf(b,sizeof(b),"%lld",(long long)luaL_checkinteger(L,2));
    lua_pushboolean(L, os_core_db_set(k,b)==0); return 1;
}
static int lua_db_geti(lua_State *L){
    const char *k=luaL_checkstring(L,1); long long def=(long long)luaL_optinteger(L,2,0); char b[32];
    if(os_core_db_get(k,b,sizeof(b))<0){ lua_pushinteger(L,(lua_Integer)def); return 1;}
    lua_pushinteger(L,(lua_Integer)strtoll(b,NULL,10)); return 1;
}

static int lua_hid_start(lua_State *L) {
    lua_pushboolean(L, os_hid_start() == ESP_OK);
    return 1;
}
static int lua_hid_stop(lua_State *L) {
    os_hid_key_release(); os_hid_mouse_release(0x03); os_hid_stop();
    return 0;
}
static int lua_hid_connected(lua_State *L) {
    lua_pushboolean(L, os_hid_connected());
    return 1;
}
/* app.hid.running(): HID 栈是否已启动 (区别于 connected: 对端是否枚举上来) */
static int lua_hid_running(lua_State *L) {
    lua_pushboolean(L, os_hid_is_running());
    return 1;
}
/* app.hid.cdc_connected(): 目标电脑的 CDC 串口是否已连接 (日志诊断页显示 USB 状态用) */
static int lua_hid_cdc_connected(lua_State *L) {
    lua_pushboolean(L, os_hid_cdc_connected());
    return 1;
}
static int lua_hid_key(lua_State *L) {
    uint8_t mod = (uint8_t)luaL_optinteger(L, 1, 0);
    uint8_t kc = (uint8_t)luaL_checkinteger(L, 2);
    os_hid_key_tap(mod, kc);
    return 0;
}
static uint8_t lua_hidc(char c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x04 + (c - 'A'));
    if (c >= '0' && c <= '9') return (uint8_t)(0x1E + (c - '0'));
    switch (c) { case ' ': return 0x2C; case '.': return 0x37; case '-': return 0x2D; case ':': return 0x33; case '/': return 0x38; default: return 0; }
}
static int lua_hid_key_str(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    for (; *s; s++) {
        char c = *s;
        uint8_t k = lua_hidc(c);
        if (!k) continue;
        uint8_t mod = (c >= 'A' && c <= 'Z') ? 0x02 : 0;
        os_hid_key_tap(mod, k);
    }
    os_hid_key_release();
    return 0;
}
static int lua_hid_mouse_move(lua_State *L) {
    int8_t dx = (int8_t)luaL_optinteger(L, 1, 0), dy = (int8_t)luaL_optinteger(L, 2, 0);
    os_hid_mouse_move(dx, dy);
    return 0;
}
static int lua_hid_mouse_click(lua_State *L) {
    uint8_t b = (uint8_t)luaL_optinteger(L, 1, 0x01);
    os_hid_mouse_click(b);
    return 0;
}
static int lua_hid_shortcut(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    struct { const char *n; uint8_t mod; uint8_t kc; } const t[]={
        {"CTRL+C",0x01,0x06},{"CTRL+V",0x01,0x19},{"CTRL+A",0x01,0x04},
        {"CTRL+X",0x01,0x1B},{"CTRL+Z",0x01,0x11},{"CTRL+Y",0x01,0x1C},
        {"CTRL+S",0x01,0x16},{"CTRL+F",0x01,0x09},{"CTRL+P",0x01,0x13},
        {"CTRL+TAB",0x01,0x2B},{"ALT+TAB",0x04,0x2B},{"WIN+R",0x08,0x15},
        {"WIN+E",0x08,0x08},{"WIN+L",0x08,0x0F},
    };
    for (size_t i=0;i<sizeof(t)/sizeof(t[0]);i++){
        if (strcasecmp(s,t[i].n)==0){ os_hid_key_tap(t[i].mod,t[i].kc); os_hid_key_release(); return 1; }
    }
    return 0;
}

/* 注册全局 app 表 */
static void lua_setup_globals(lua_State *L) {
    lua_createtable(L, 0, 40);
    /* 常量 */
    lua_pushinteger(L, 400);           lua_setfield(L, -2, "W");
    lua_pushinteger(L, 300);           lua_setfield(L, -2, "H");
    lua_pushinteger(L, 0);             lua_setfield(L, -2, "BLACK");
    lua_pushinteger(L, 1);             lua_setfield(L, -2, "WHITE");
    /* 按键常量 (与 os_action_t 数值对应): UP/DOWN/LEFT/RIGHT/OK(CONFIRM)/BACK/HOME */
    lua_pushinteger(L, 1);             lua_setfield(L, -2, "UP");
    lua_pushinteger(L, 2);             lua_setfield(L, -2, "DOWN");
    lua_pushinteger(L, 3);             lua_setfield(L, -2, "LEFT");
    lua_pushinteger(L, 4);             lua_setfield(L, -2, "RIGHT");
    lua_pushinteger(L, 5);             lua_setfield(L, -2, "OK");
    lua_pushinteger(L, 5);             lua_setfield(L, -2, "CONFIRM");
    lua_pushinteger(L, 6);             lua_setfield(L, -2, "BACK");
    lua_pushinteger(L, 7);             lua_setfield(L, -2, "HOME");
    /* 绘制 */
    lua_pushcfunction(L, lua_clear);     lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, lua_px);        lua_setfield(L, -2, "px");
    lua_pushcfunction(L, lua_fill);      lua_setfield(L, -2, "fill");
    lua_pushcfunction(L, lua_rect);      lua_setfield(L, -2, "store_rect");
    lua_pushcfunction(L, lua_rect);      lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, lua_hline);     lua_setfield(L, -2, "hline");
    lua_pushcfunction(L, lua_vline);     lua_setfield(L, -2, "vline");
    lua_pushcfunction(L, lua_text);      lua_setfield(L, -2, "text");
    lua_pushcfunction(L, lua_textc);     lua_setfield(L, -2, "textc");
    lua_pushcfunction(L, lua_textw);     lua_setfield(L, -2, "textw");
    lua_pushcfunction(L, lua_big);       lua_setfield(L, -2, "big");
    lua_pushcfunction(L, lua_bigc);      lua_setfield(L, -2, "bigc");
    lua_pushcfunction(L, lua_zh16);     lua_setfield(L, -2, "zh");
    lua_pushcfunction(L, lua_zh24);     lua_setfield(L, -2, "zh24");
    lua_pushcfunction(L, lua_bmp);      lua_setfield(L, -2, "bmp");
    lua_pushcfunction(L, lua_bmpr);     lua_setfield(L, -2, "bmpr");
    lua_pushcfunction(L, lua_zh);       lua_setfield(L, -2, "zhs");
    /* 固件字模族 (见上方注释; Lua 应用一律用固件字体, 不自绘) */
    lua_pushcfunction(L, lua_texts);    lua_setfield(L, -2, "texts");
    lua_pushcfunction(L, lua_textm);    lua_setfield(L, -2, "textm");
    lua_pushcfunction(L, lua_textsb);   lua_setfield(L, -2, "textsb");
    lua_pushcfunction(L, lua_zhsm);     lua_setfield(L, -2, "zhsm");
    lua_pushcfunction(L, lua_zhsb);     lua_setfield(L, -2, "zhsb");
    lua_pushcfunction(L, lua_mix);      lua_setfield(L, -2, "mix");
    lua_pushcfunction(L, lua_mixc);     lua_setfield(L, -2, "mixc");
    lua_pushcfunction(L, lua_mixw);     lua_setfield(L, -2, "mixw");
    /* 时间/输入 */
    lua_pushcfunction(L, lua_millis);    lua_setfield(L, -2, "millis");
    lua_pushcfunction(L, lua_key);       lua_setfield(L, -2, "key");
    lua_pushcfunction(L, lua_delay);     lua_setfield(L, -2, "delay");
    /* 声音/震动 */
    lua_pushcfunction(L, lua_tone);      lua_setfield(L, -2, "tone");
    lua_pushcfunction(L, lua_vibrate);   lua_setfield(L, -2, "vibrate");
    /* 退出 */
    lua_pushcfunction(L, lua_exit);      lua_setfield(L, -2, "exit");
    lua_pushcfunction(L, lua_redraw);    lua_setfield(L, -2, "redraw");
    lua_pushcfunction(L, lua_sbar_invert); lua_setfield(L, -2, "sbar");
    /* 触摸 */
    lua_pushcfunction(L, lua_app_touch);     lua_setfield(L, -2, "touch");
    lua_pushcfunction(L, lua_touch_start);   lua_setfield(L, -2, "touch_start");
    lua_pushcfunction(L, lua_touch_hold);    lua_setfield(L, -2, "touch_hold");
    /* 系统信息 app.sys */
    lua_newtable(L);
    lua_pushcfunction(L, lua_sys_battery); lua_setfield(L, -2, "battery");
    lua_pushcfunction(L, lua_sys_heap);    lua_setfield(L, -2, "heap_free");
    lua_pushcfunction(L, lua_sys_time);    lua_setfield(L, -2, "time_ms");
    lua_pushcfunction(L, lua_sys_clock);   lua_setfield(L, -2, "clock");
    lua_setfield(L, -2, "sys");
    /* 文件 app.fs */
    lua_newtable(L);
    lua_pushcfunction(L, lua_fs_exists);  lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, lua_fs_read);    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, lua_fs_write);   lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_fs_append);  lua_setfield(L, -2, "append");
    lua_pushcfunction(L, lua_fs_remove);  lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, lua_fs_listdir); lua_setfield(L, -2, "listdir");
    lua_setfield(L, -2, "fs");
    /* 键值 app.nvs */
    lua_newtable(L);
    lua_pushcfunction(L, lua_nvs_get_int); lua_setfield(L, -2, "get_int");
    lua_pushcfunction(L, lua_nvs_set_int); lua_setfield(L, -2, "set_int");
    lua_pushcfunction(L, lua_nvs_get_str); lua_setfield(L, -2, "get_str");
    lua_pushcfunction(L, lua_nvs_set_str); lua_setfield(L, -2, "set_str");
    lua_setfield(L, -2, "nvs");
    /* 网络 app.net */
    lua_newtable(L);
    lua_pushcfunction(L, lua_net_is_connected); lua_setfield(L, -2, "is_connected");
    lua_pushcfunction(L, lua_net_get_ip);       lua_setfield(L, -2, "get_ip");
    lua_pushcfunction(L, lua_net_http_get);     lua_setfield(L, -2, "http_get");
    lua_pushcfunction(L, lua_net_ping);         lua_setfield(L, -2, "ping");
    lua_pushcfunction(L, lua_net_port);         lua_setfield(L, -2, "port");
    lua_pushcfunction(L, lua_net_arp);          lua_setfield(L, -2, "arp");
    lua_pushcfunction(L, lua_net_arp_list);     lua_setfield(L, -2, "arp_list");
    lua_pushcfunction(L, lua_net_ipinfo);       lua_setfield(L, -2, "ipinfo");
    lua_pushcfunction(L, lua_net_vendor);       lua_setfield(L, -2, "vendor");
    lua_setfield(L, -2, "net");
    /* 无线探测 app.wifi */
    lua_newtable(L);
    lua_pushcfunction(L, lua_wifi_init);      lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_wifi_scan);      lua_setfield(L, -2, "scan");
    lua_pushcfunction(L, lua_wifi_scan_done); lua_setfield(L, -2, "scan_done");
    lua_pushcfunction(L, lua_wifi_ap_count);  lua_setfield(L, -2, "ap_count");
    lua_pushcfunction(L, lua_wifi_ap_info);   lua_setfield(L, -2, "ap_info");
    lua_pushcfunction(L, lua_wifi_probe_init);   lua_setfield(L, -2, "probe_init");
    lua_pushcfunction(L, lua_wifi_probe_stop);   lua_setfield(L, -2, "probe_stop");
    lua_pushcfunction(L, lua_wifi_probe_count);  lua_setfield(L, -2, "probe_count");
    lua_pushcfunction(L, lua_wifi_probe_info);   lua_setfield(L, -2, "probe_info");
    lua_pushcfunction(L, lua_wifi_release);      lua_setfield(L, -2, "release");
    lua_pushcfunction(L, lua_wifi_ch_stat);      lua_setfield(L, -2, "ch_stat");
    lua_pushcfunction(L, lua_wifi_cur_ssid);     lua_setfield(L, -2, "cur_ssid");
    lua_pushcfunction(L, lua_wifi_connecting);   lua_setfield(L, -2, "connecting");
    lua_pushcfunction(L, lua_wifi_connect);      lua_setfield(L, -2, "connect");
    lua_pushcfunction(L, lua_wifi_clear_saved);  lua_setfield(L, -2, "clear_saved");
    lua_setfield(L, -2, "wifi");
    /* 电脑键鼠 app.hid */
    lua_newtable(L);
    lua_pushcfunction(L, lua_hid_start);      lua_setfield(L, -2, "start");
    lua_pushcfunction(L, lua_hid_stop);       lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, lua_hid_connected);  lua_setfield(L, -2, "connected");
    lua_pushcfunction(L, lua_hid_running);    lua_setfield(L, -2, "running");
    lua_pushcfunction(L, lua_hid_key);        lua_setfield(L, -2, "key");
    lua_pushcfunction(L, lua_hid_key_str);    lua_setfield(L, -2, "key_str");
    lua_pushcfunction(L, lua_hid_mouse_move); lua_setfield(L, -2, "mouse_move");
    lua_pushcfunction(L, lua_hid_mouse_click);lua_setfield(L, -2, "mouse_click");
    lua_pushcfunction(L, lua_hid_shortcut);   lua_setfield(L, -2, "shortcut");
    lua_pushcfunction(L, lua_hid_cdc_read);   lua_setfield(L, -2, "cdc_read");
    lua_pushcfunction(L, lua_hid_cdc_connected); lua_setfield(L, -2, "cdc_connected");
    lua_setfield(L, -2, "hid");
    /* 核心互通数据库 app.db (nvs 命名空间 core_db) */
    lua_newtable(L);
    lua_pushcfunction(L, lua_db_set);  lua_setfield(L, -2, "set");
    lua_pushcfunction(L, lua_db_get);  lua_setfield(L, -2, "get");
    lua_pushcfunction(L, lua_db_del);  lua_setfield(L, -2, "del");
    lua_pushcfunction(L, lua_db_seti); lua_setfield(L, -2, "seti");
    lua_pushcfunction(L, lua_db_geti); lua_setfield(L, -2, "geti");
    lua_setfield(L, -2, "db");
    /* 软键盘 kbd_lib 统一入口 (见上方注释) */
    lua_newtable(L);
    lua_pushcfunction(L, lua_kbd_use);       lua_setfield(L, -2, "use");
    lua_pushcfunction(L, lua_kbd_reset);     lua_setfield(L, -2, "reset");
    lua_pushcfunction(L, lua_kbd_draw);      lua_setfield(L, -2, "draw");
    lua_pushcfunction(L, lua_kbd_hit);       lua_setfield(L, -2, "hit");
    lua_pushcfunction(L, lua_kbd_geom);      lua_setfield(L, -2, "geom");
    lua_pushcfunction(L, lua_kbd_height);    lua_setfield(L, -2, "height");
    lua_pushcfunction(L, lua_kbd_count);     lua_setfield(L, -2, "count");
    lua_pushcfunction(L, lua_kbd_press);     lua_setfield(L, -2, "press");
    lua_pushcfunction(L, lua_kbd_char);      lua_setfield(L, -2, "char");
    lua_pushcfunction(L, lua_kbd_label);     lua_setfield(L, -2, "label");
    lua_pushcfunction(L, lua_kbd_layer);     lua_setfield(L, -2, "layer_on");
    lua_pushcfunction(L, lua_kbd_get);       lua_setfield(L, -2, "get");
    lua_pushcfunction(L, lua_kbd_set);       lua_setfield(L, -2, "set");
    {   /* 动作码 + 几何 + 样式常量 */
        static const struct { const char *n; int v; } ka[] = {
            {"CHAR",KBD_ACT_CHAR},{"ENTER",KBD_ACT_ENTER},{"BKSP",KBD_ACT_BKSP},
            {"SPACE",KBD_ACT_SPACE},{"ESC",KBD_ACT_ESC},{"TAB",KBD_ACT_TAB},
            {"UP",KBD_ACT_UP},{"DOWN",KBD_ACT_DOWN},{"LEFT",KBD_ACT_LEFT},
            {"RIGHT",KBD_ACT_RIGHT},{"CAPS",KBD_ACT_CAPS},{"SHIFT",KBD_ACT_SHIFT},
            {"FN",KBD_ACT_FN},{"CTRL",KBD_ACT_CTRL},{"ALT",KBD_ACT_ALT},
            {"ROWS",KBD_ROWS},{"ROW",KBD_ROW},{"ROW_EDIT",KBD_EDIT_ROW},{"H",KBD_H},
            {"STYLE_STD",KBD_STYLE_STD},{"STYLE_EDIT",KBD_STYLE_EDIT},
            {"STYLE_ROWS",KBD_STYLE_ROWS},{"STYLE_DIALOG",KBD_STYLE_DIALOG},
        };
        for (size_t i = 0; i < sizeof(ka)/sizeof(ka[0]); i++) {
            lua_pushinteger(L, ka[i].v);
            lua_setfield(L, -2, ka[i].n);
        }
    }
    lua_setfield(L, -2, "kbd");
    /* 弹窗模块 app.dlg (框架统一: 见上方 lua_dlg_toast 注释) */
    lua_newtable(L);
    lua_pushcfunction(L, lua_dlg_toast); lua_setfield(L, -2, "toast");
    lua_setfield(L, -2, "dlg");
    lua_setglobal(L, "app");
}

/* 读取 SD 卡脚本到 PSRAM 并执行; 成功返回 true */
static bool lua_load_run(const char *path) {
    lua_probe_stage(5, "load_run 入口");
    if (s_L) lua_close(s_L);
    s_L = lua_newstate(lua_psram_alloc, NULL, (unsigned)esp_timer_get_time());
    if (!s_L) { ESP_LOGE(TAG, "lua_newstate 失败"); return false; }
    lua_probe_stage(6, "newstate 成功");
    luaL_openlibs(s_L);
    lua_setup_globals(s_L);
    lua_probe_stage(7, "openlibs+bridge 成功");

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "打开失败: %s", path);
        snprintf(s_err, sizeof(s_err), "\xe6\x97\xa0\xe6\xb3\x95\xe6\x89\x93\xe5\xbc\x80"); /* 无法打开 */
        s_err_pending = true; lua_close(s_L); s_L = NULL; return false; }
    lua_probe_stage(8, "fopen 成功");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); ESP_LOGE(TAG, "空文件: %s", path);
        snprintf(s_err, sizeof(s_err), "\xe7\xa9\xba\xe6\x96\x87\xe4\xbb\xb6"); /* 空文件 */
        s_err_pending = true; lua_close(s_L); s_L = NULL; return false; }
    char *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); ESP_LOGE(TAG, "PSRAM 分配 %ldB 失败", sz);
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
        ESP_LOGE(TAG, "执行失败: %s", m ? m : "(null)");
        snprintf(s_err, sizeof(s_err), "%s", m ? m : "Lua error");
        s_err_pending = true;
        lua_close(s_L); s_L = NULL;
        return false;
    }
    lua_probe_stage(11, "pcall 完成 (脚本体跑完)");
    s_err_pending = false;
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
        const char *m = lua_tostring(s_L, -1);
        ESP_LOGE(TAG, "Lua %s() 错误: %s", name, m ? m : "(null)");
        snprintf(s_err, sizeof(s_err), "%s: %s", name, m ? m : "?");
        s_err_pending = true;
        lua_stop(ctx);   /* 出错 → 回列表 (内部已清 L 并重置 pane) */
        return;
    }
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
        const char *m = lua_tostring(s_L, -1);
        ESP_LOGE(TAG, "Lua action() 错误: %s", m ? m : "(null)");
        snprintf(s_err, sizeof(s_err), "action: %s", m ? m : "?");
        s_err_pending = true;
        lua_stop(ctx);
        return false;
    }
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
                ESP_LOGW(TAG, "on_exit 错误: %s", lua_tostring(s_L, -1) ? lua_tostring(s_L, -1) : "(null)");
        }
        lua_close(s_L);
    }
    s_L = NULL;
    s_mode = LUA_MODE_LIST;
    s_exit_req = false;
    os_notify_tone_stop();
    if (ctx) {
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
    s_keys_head = s_keys_tail = 0;
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
    s_keys_head = s_keys_tail = 0;
    s_exit_req = false;
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
    if (!s_pane.loaded) {
        s_pane.root = LUA_ROOT_PATH;
        s_pane.is_item = is_lua;
        s_pane.on_select = lua_pane_select;
        s_pane.list_y = 20;
    }
    os_pane_reset(&s_pane);
    s_pane.focus = 1;   /* 直接进右栏 */
    os_pane_build_right(&s_pane);
    ctx->needs_redraw = true;
}

static void lua_render(ui_ctx_t *ctx) {
    if (!ctx->lcd) return;
    s_lcd = ctx->lcd;
    if (s_mode == LUA_MODE_RUN) {
        lua_call_optional(ctx, "render", -1);   /* 出错已内部回列表 */
    } else {
        st7305_clear(ctx->lcd, ST7305_COLOR_WHITE);
        os_pane_render(ctx, &s_pane);
    }
}

static void lua_action(ui_ctx_t *ctx, os_action_t a) {
    s_lcd = ctx->lcd;
    if (s_mode == LUA_MODE_LIST) {
        if (os_pane_action(ctx, &s_pane, a)) return;
        if (a == OS_ACTION_BACK) os_pop(ctx);
        return;
    }
    /* RUN 态: 投进 FIFO + 转发 action() */
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
        os_pop(ctx);                                   /* 未消费 → 退出应用(返回上层) */
        return;
    }
    lua_call_optional(ctx, "action", (int)a);   /* 按键值作为参数传给 Lua 的 action(a) */
    if (ctx) ctx->needs_redraw = true;      /* 按键后立即重绘, 否则 RUN 态不刷新 */
    if (s_mode != LUA_MODE_RUN) {
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
        return;
    }
    if (s_exit_req) { s_exit_req = false; lua_stop(ctx); }
}

static bool lua_touch(ui_ctx_t *ctx, int x, int y) {
    /* RUN 态不消费触摸: 脚本自己在 poll()/render() 里调用 app.touch() 取坐标绘制,
     * 页面层不介入, 否则会与脚本的绘制/状态机抢同一次触摸事件。 */
    if (s_mode == LUA_MODE_LIST) return os_pane_touch(ctx, &s_pane, x, y);
    return false;
}

static void lua_poll(ui_ctx_t *ctx) {
    if (s_mode == LUA_MODE_LIST) { os_pane_poll(ctx, &s_pane); return; }
    /* RUN 态: 先处理待办, 再转给 Lua poll() */
    if (s_exit_req) { s_exit_req = false; lua_stop(ctx); return; }
    if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
    if (s_L) lua_call_optional(ctx, "poll", -1);
    if (s_mode == LUA_MODE_RUN) s_lua_ctx = ctx;
    if (s_mode == LUA_MODE_RUN && ctx) ctx->needs_redraw = true;   /* 逐帧重绘: 支撑动画/触屏 */
    if (s_mode != LUA_MODE_RUN) s_lua_ctx = NULL;
    if (s_mode != LUA_MODE_RUN) {
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
    }
}

static void lua_on_exit(ui_ctx_t *ctx) {
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
