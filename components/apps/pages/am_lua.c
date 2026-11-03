/**
 * am_lua.c — Lua 应用的**运行入口与门禁**（不再负责扫描/列表）
 *
 * ════════════════════════════════════════════════════════════════════════
 * 2026-11-03 起的分工
 * ------------------------------------------------------------------------
 *   发现 / 身份 / 安装状态 → `am_app_core`（唯一来源）
 *   启动（设 boot 路径 + need=wifi 门禁 + 进 Lua 页）→ 本文件
 *
 * 为什么不再自己扫：原来 `am_lua_scan` 与商店的 `am_pkg_scan` **扫同一批目录**，
 * 各建一张表、各判一次"装没装"，于是同一个应用在应用中心与商店里是两条记录、
 * 两种状态。现在扫描只剩一份（核心），本文件只回答"这一条怎么跑起来"。
 *
 * 保留的磁盘动作为两件（都只做一次、与"发现"无关）：
 *   · 首次使用自动建好 /sdcard/apps 等目录（用户不用手动建）；
 *   · 旧布局 → 新布局的一次性迁移（rename 同分区瞬间完成）。
 * ════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "am_internal.h"
#include "am_app_core.h"
#include "apppack.h"      /* apppack_parse: 现读 need=wifi */
#include "wifi_manager.h" /* is_connected/is_connecting: need=wifi 门禁 */

#define TAG "AMLUA"

/* ==================== Wi-Fi 门禁 (LAPP `--@app need=wifi`) ====================
 * 用户规范 (2026-10-02): "如果某个软件声明需要 Wi-Fi, 打开软件时弹窗提醒需要 Wi-Fi,
 *   用内置连接 Wi-Fi 弹窗进行连接; 若点取消没连上, 这个软件又必须用 Wi-Fi, 就退出它。"
 *
 * 流程: 点开 → 未联网 → 确认框 ─┬─ 确定 → 复用系统"连接WiFi"(扫描+密码键盘)
 *                              │        → 连上后**继续把应用启动起来**
 *                              └─ 取消 → 不启动 (toast)
 *       连网超时 / 用户中途退出连网流程 → 同样不启动。
 *
 * ⚠ 不要自己 os_hw_request(OS_HW_WIFI): settings_wifi_connect_open() 内部会按需
 *   request 并记下"是我们开的"私有标志, 用户取消时才有 release。这里先 request
 *   会让它以为 WiFi 早已开着 → 不记标志 → 取消后再也没人关 (射频常开)。
 * ⚠ 轮询必须走全局 service: 连网期间用户可能回到桌面/别的页面, 应用管家的 poll
 *   那时根本不执行; 而 service tick 每帧都跑。 */
#define WL_GATE_TIMEOUT_MS  120000u     /* 等待联网上限 (2 分钟) */
static char     s_wg_path[192];         /* 待启动脚本路径 ("" = 无) */
static bool     s_wg_ask     = false;   /* 确认框已弹 (防重复点按) */
static bool     s_wg_wait    = false;   /* 等 Wi-Fi 连接中 */
static bool     s_wg_connect = false;   /* 下一帧开"连接WiFi"弹窗 */
static uint32_t s_wg_t0      = 0;

static void wl_gate_abort(ui_ctx_t *ctx) {
    ESP_LOGI(TAG, "gate abort (ask=%d wait=%d conn=%d)", s_wg_ask, s_wg_wait, s_wg_connect);
    s_wg_path[0] = 0; s_wg_ask = false; s_wg_wait = false; s_wg_connect = false;
    os_dialog_toast(ctx, "\xe8\xaf\xa5\xe5\xba\x94\xe7\x94\xa8\xe9\x9c\x80\xe8\xa6\x81 Wi-Fi"); /* 该应用需要 Wi-Fi */
}
/* 连上 → 这时才真正启动 (保证应用拿到的是已联网状态) */
static void wl_gate_launch(ui_ctx_t *ctx) {
    char p[192];
    snprintf(p, sizeof(p), "%s", s_wg_path);
    s_wg_path[0] = 0; s_wg_ask = false; s_wg_wait = false; s_wg_connect = false;
    if (!p[0]) return;
    os_dialog_clear_all(ctx);           /* 收掉"正在扫描/输密码"等中间弹窗 */
    ESP_LOGI(TAG, "Wi-Fi 已连接 → 启动 %s", p);
    os_page_lua_set_boot(p);
    os_push(ctx, OS_PAGE_LUA);
}
static void wl_gate_confirm_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    s_wg_ask = false;
    ESP_LOGI(TAG, "gate confirm cb: result=%d", result);
    if (result != 0) { wl_gate_abort(ctx); return; }   /* 取消 → 不启动 */
    s_wg_connect = true;                /* 确定 → 下一帧打开**系统网络模块**的连接界面 */
}
static void wl_gate_tick(ui_ctx_t *ctx) {
    if (s_wg_connect) {                 /* 用户点了"去设置" → 下一帧开系统连网流程 */
        s_wg_connect = false;
        s_wg_t0 = (uint32_t)(esp_timer_get_time() / 1000);
        s_wg_wait = true;               /* 配完连上后继续把应用启动起来 */
        settings_wifi_connect_open(ctx);   /* ★ 唯一入口: 系统网络模块的连接流程 */
        return;
    }
    if (!s_wg_wait) {
        /* 确认框被**不回调的路径**清掉时（os_dialog_clear_all / 屏保回主页 /
         * 直接 os_dialog_pop 都不走回调），s_wg_ask 会永远卡 true → 之后所有点按被吞。 */
        if (s_wg_ask && os_dialog_depth() <= 0) {
            ESP_LOGW(TAG, "gate 自愈: 确认框已被外部清掉, 复位门禁状态");
            s_wg_ask = false;
            s_wg_path[0] = 0;
        }
        return;
    }
    if (wifi_manager_is_connected()) { wl_gate_launch(ctx); return; }
    /* 还在连(扫描/输密码/关联) 或有弹窗在 → 继续等; 否则=用户退出了连网流程 */
    bool alive = wifi_manager_is_connecting() || os_dialog_depth() > 0;
    if (!alive || (uint32_t)(esp_timer_get_time() / 1000) - s_wg_t0 > WL_GATE_TIMEOUT_MS) {
        ESP_LOGI(TAG, "Wi-Fi 门禁: 未连上(%s), 不启动", alive ? "超时" : "已退出");
        wl_gate_abort(ctx);
    }
}
static const os_service_t s_svc_lua_gate = { .name = "lua_wifi_gate", .tick = wl_gate_tick };
void am_lua_gate_register(void) { os_register_service(&s_svc_lua_gate); }

/* 门禁 + 启动的唯一落地处（**联网归网络模块** —— 本函数不自己联网，
 * 只做两件事: ① 没配过网络 → 引导去设置; ② 配过 → 等模块连上再启动）。 */
static void wl_open(ui_ctx_t *ctx, const char *path, bool need_wifi) {
    if (!path || !path[0]) return;
    if (need_wifi && !wifi_manager_is_connected()) {
        if (s_wg_ask || s_wg_wait) return;           /* 已在门禁流程中 → 吞掉重复点按 */
        snprintf(s_wg_path, sizeof(s_wg_path), "%s", path);
        if (!wifi_manager_has_saved()) {
            ESP_LOGI(TAG, "Wi-Fi 门禁: %s 需要联网且从未配置 → 引导设置", path);
            s_wg_ask = true;
            os_dialog_confirm_ex(ctx,
                "\xe8\xaf\xa5\xe5\xba\x94\xe7\x94\xa8\xe9\x9c\x80\xe8\xa6\x81 Wi-Fi\xef\xbc\x8c\xe6\x98\xaf\xe5\x90\xa6\xe5\x8e\xbb\xe8\xae\xbe\xe7\xbd\xae\xe8\x81\x94\xe7\xbd\x91\xef\xbc\x9f", /* 该应用需要 Wi-Fi，是否去设置联网？ */
                0, -1, wl_gate_confirm_cb, NULL);
            return;
        }
        ESP_LOGI(TAG, "Wi-Fi 门禁: %s 需要联网, 等网络模块连上", path);
        s_wg_wait = true;
        s_wg_t0 = (uint32_t)(esp_timer_get_time() / 1000);
        os_dialog_toast_ms(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5\xe7\xbd\x91\xe7\xbb\x9c...", 1500); /* 正在连接网络... */
        return;
    }
    os_page_lua_set_boot(path);
    os_push(ctx, OS_PAGE_LUA);
}

/* 启动一个条目（应用管家与主菜单共用）：
 *   路径来自**统一核心**的 entry（Lua 文件 / LPK 的 registry entry），
 *   所以 Lua 与 LPK 应用走同一段代码，不再两套判断。 */
bool am_lua_open_if_entry(ui_ctx_t *ctx, const os_app_t *a)
{
    lua_probe_stage(1, "open_if_entry 入口");
    const am_app_t *e = am_core_from_view(a);
    /* ★ 修复 2026-11-03: 内置 C 应用也由统一核心管理(entry 被写成 "page:N" 哨兵),
     *   但必须走调用方的 os_push(a->page), 绝不能走 wl_open —— 否则 wl_open 会把
     *   "page:N" 设成 Lua 启动路径并 push Lua 页, 随后 lua_load_run 去 fopen("page:N")
     *   失败, 屏幕上弹「无法打开」(所有内置 C 应用全部中招)。
     *   只有 entry 是真实文件/包路径的 Lua / LPK 项才走 wl_open。
     *   (am_lua_is_entry() 已按此规则排除 builtin, 这里补上同一道闸。) */
    if (e && e->kind != AM_KIND_BUILTIN && e->entry[0]) {
        lua_probe_stage(2, e->kind == AM_KIND_LPK ? "LPK 应用命中" : "Lua 项命中");
        wl_open(ctx, e->entry, e->need_wifi);
        return true;
    }
    /* 内置 C 应用或无法识别的条目 → 交给调用方 os_push(a->page) */
    return false;
}

/* 兼容旧入口：只给路径（商店/别的模块）→ 现读包体拿 need=wifi 再走同一套门禁。 */
void am_lua_open_path(ui_ctx_t *ctx, const char *path) {
    bool need = false;
    char p[192];
    snprintf(p, sizeof(p), "%s", path ? path : "");
    if (!p[0]) return;
    for (int i = 0; i < am_core_count(); i++) {
        const am_app_t *e = am_core_at(i);
        if (e && e->entry[0] && strcmp(e->entry, p) == 0) { need = e->need_wifi; goto go; }
    }
    {
        apppack_meta_t m;
        need = (apppack_parse(p, &m) == ESP_OK) && m.need_wifi;
    }
go:
    wl_open(ctx, p, need);
}

/* 是否 Lua/LPK 项（回填启动路径）。供需要"先问路径"的调用方用。 */
bool am_lua_is_entry(const os_app_t *a, const char **path_out)
{
    const am_app_t *e = am_core_from_view(a);
    if (!e || e->kind == AM_KIND_BUILTIN || !e->entry[0]) return false;
    if (path_out) *path_out = e->entry;
    return true;
}

/* ==================== 目录准备（与"发现"无关，一次而已） ==================== */

/* ★ TF 卡首次使用/换新卡时自动建好分类目录，用户不用手动建。
 *   /sdcard/apps/{engine|app|ops} —— 与安装目标目录一致。幂等。 */
/* ★ 旧布局 → 新布局（按类型分二级目录）。rename 同分区瞬间完成；
 *   目标已存在则跳过（不覆盖）。老卡插上第一次进应用管家就自动搬好。 */
static void am_migrate_tf_layout(void)
{
    static const struct { const char *old; const char *newp; } M[] = {
        { "/sdcard/gb",          "/sdcard/roms/gb"          },
        { "/sdcard/gbc",         "/sdcard/roms/gbc"         },
        { "/sdcard/nes",         "/sdcard/roms/nes"         },
        { "/sdcard/md",          "/sdcard/roms/md"          },
        { "/sdcard/sms",         "/sdcard/roms/sms"         },
        { "/sdcard/AB",          "/sdcard/roms/ab"          },
        { "/sdcard/lava",        "/sdcard/roms/lava"        },
        { "/sdcard/vpet",        "/sdcard/roms/vpet"        },
        { "/sdcard/gam",         "/sdcard/roms/gam"         },
        { "/sdcard/mp3",         "/sdcard/media/mp3"        },
        { "/sdcard/screensaver", "/sdcard/media/screensaver"},
        { "/sdcard/dict",        "/sdcard/data/saves"       },
        { "/sdcard/log",         "/sdcard/data/log"         },
        { "/sdcard/App",         "/sdcard/apps/data"        },
    };
    struct stat st;
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++) {
        if (stat(M[i].old, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (stat(M[i].newp, &st) == 0) {
            ESP_LOGW(TAG, "迁移跳过 %s (目标已存在)", M[i].old);
            continue;
        }
        if (rename(M[i].old, M[i].newp) == 0) ESP_LOGI(TAG, "迁移 %s → %s", M[i].old, M[i].newp);
        else                                 ESP_LOGW(TAG, "迁移失败 %s", M[i].old);
    }
}

static void am_lua_ensure_dirs(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    mkdir("/sdcard/apps", 0777);
    mkdir("/sdcard/apps/engine", 0777);
    mkdir("/sdcard/apps/app", 0777);
    mkdir("/sdcard/apps/ops", 0777);
    mkdir("/sdcard/apps/data", 0777);
    mkdir("/sdcard/roms", 0777);
    mkdir("/sdcard/media", 0777);
    mkdir("/sdcard/data", 0777);
    am_migrate_tf_layout();
}

/* 开机调一次：建目录 + 踢一次统一核心扫描（扫描结果会注册进 os_app，
 * 桌面图标才认得出来）。原来是"扫 Lua 并注册"，现在只剩这一句。 */
void am_lua_register_boot(void)
{
    am_lua_ensure_dirs();
    am_core_boot();
    ESP_LOGI(TAG, "启动: 目录就绪, 统一应用核心已开始扫描");
}
