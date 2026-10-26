/* ============================================================
 * am_lua.c — 应用中心 · Lua 单文件程序扫描模块
 * (2026-09-20 从 page_app_manager.c 拆出)
 *
 * 职责: 后台扫描 SPIFFS /store/apps 与 TF /sdcard/apps 下 {engine|app|ops}
 *       分类目录里的 .lua 单文件程序, 按分类并入应用列表 (免烧录运行)。
 * 扫描在后台任务里做, UI 线程经 am_lua_tick() 取货 (影子缓冲 → 正式缓冲)。
 * ============================================================ */
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
#include "apppack.h"   /* LAPP 单文件资源包: 取名称/简介/分类/图标 */
#include "wifi_manager.h" /* is_connected/is_connecting: need=wifi 门禁 */
#include "ui_common.h" /* ui_main_icon_bitmap: LAPP icon= 反查主图标表下标 */

#define TAG "AMLUA"

/* 扫描状态 (原声明在商店块, 拆分后归本模块) */
/* ★★★ 2026-10-25 linit「切换分类要等接近1秒」:
 *   原来 s_lua 只有**一份**数据、且按 s_cat 逐分类扫 —— 切一次分类就要整趟重扫
 *   (opendir + 每个 .lua 读包头的 SD I/O), 而且来回切会反复重扫。
 *   现在: **一次把 engine/app/ops 三个目录全扫进来**, 每条带 fid 标记,
 *   切分类只做内存过滤 ⇒ 切换立刻出列表, 不再等 SD。 */
static bool s_lua_scanned = false;    /* 三个目录已扫完 (数据可用) */

/* ========== Lua 文件项 (应用管家接管 Lua 启动) ==========
 * 扫描 /store/apps 与 /sdcard/apps 下 engine/game/app 三分目录, 合并去重
 * (同文件名 TF/SD 优先), 按分类显示到 引擎/游戏/应用. 选中即交给 Lua 引擎运行.
 * 独立 Lua 启动器已移除, 启动 Lua 的责任统一由本应用管家承担. */
#define AM_LUA_MAX   48
typedef struct {
    os_app_t app;
    char     id[48];
    char     label[48];
    char     path[160];
    char     desc[96];      /* LAPP 简介 (无打包则空) */
    char     cat[16];       /* LAPP 分类 (无打包则空) —— app.cat 指向这里 */
    int      icon_idx;      /* LAPP icon= 反查主图标表得到的下标 */
    bool     need_wifi;     /* LAPP `need=wifi`: 启动前须联网 (见文件末门禁) */
    uint8_t  fid;           /* 来自哪个目录: 0=engine 1=app 2=ops (切分类时按它过滤) */
} am_lua_entry_t;
static am_lua_entry_t s_lua[AM_LUA_MAX];
static int s_lua_n = 0;
/* 分类 Lua 扫描后台化 (V1.6.x): 前台重建时不再同步扫 SPIFFS, 先显示已组装好的 C 应用,
 * 后台任务扫完写入影子缓冲 s_lua_bg, UI 线程在 poll 里一次性搬到 s_lua 再重建填充.
 * s_lua_scan_cat: 正在/已扫的分类目标; s_lua_ok_cat: s_lua 数据当前有效的分类. */
static am_lua_entry_t s_lua_bg[AM_LUA_MAX];   /* 后台扫描影子缓冲 */
static int            s_lua_bg_n = 0;
static volatile bool  s_lua_busy = false;     /* 后台扫描进行中 */
static volatile bool  s_lua_done = false;     /* 后台扫描完成 (UI 取货) */

/* 该应用指针是否为 Lua 文件项; 是则回填完整路径 */
bool am_lua_is_entry(const os_app_t *a, const char **path_out) {
    for (int i = 0; i < s_lua_n; i++)
        if (&s_lua[i].app == a) { if (path_out) *path_out = s_lua[i].path; return true; }
    return false;
}
/* 同上, 但返回下标 (-1 = 不是 Lua 项)。门禁需要"记住待启动的是哪一项"。 */
static int am_lua_index(const os_app_t *a) {
    for (int i = 0; i < s_lua_n; i++) if (&s_lua[i].app == a) return i;
    return -1;
}

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
 *   那时根本不执行; 而 service tick 每帧都跑。
 * ⚠ 开连网弹窗**不在确认框回调里做**, 而是挂到下一帧的 tick: 弹窗回调返回后
 *   os_dialog 还要收掉当前框, 同期再 push 容易互相顶掉。 */
#define WL_GATE_TIMEOUT_MS  120000u     /* 等待联网上限 (2 分钟) */
static char     s_wg_path[160];         /* 待启动脚本路径 ("" = 无) */
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
    char p[160];
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
        /* ★★ 2026-10-15 自愈 (修"弹窗自动关闭后再点没反应"): 确认框被**不回调的路径**
         *   清掉时 (os_dialog_clear_all / 屏保回主页 / 直接 os_dialog_pop 都不走回调),
         *   s_wg_ask 会永远卡 true → 之后所有点按被吞。发现"ask 但弹窗已不在"就地复位。 */
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

/* 门禁 + 启动的唯一落地处 (2026-10-15 linit 定: **联网归网络模块** —— 本函数不再自己
 * 联网/弹连接流程, 只做两件事: ① 没配过网络 → 引导去设置; ② 配过 → 等模块连上再启动)。 */
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

/* 启动 Lua 文件项 (应用管家与主菜单共用): 先设 boot 路径再进 Lua 页。
 * 主菜单直接 os_push(OS_PAGE_LUA) 会拿不到路径 → 打开空白/上次的程序, 所以必须走这里。
 * 声明了 `need=wifi` 的应用在未联网时**不直接进**, 改为弹窗 → 连网 → 连上再进。 */
bool am_lua_open_if_entry(ui_ctx_t *ctx, const os_app_t *a) {
    lua_probe_stage(1, "open_if_entry 入口");
    int i = am_lua_index(a);
    if (i < 0) return false;
    lua_probe_stage(2, "Lua 项命中");
    wl_open(ctx, s_lua[i].path, s_lua[i].need_wifi);
    return true;
}
/* 按路径启动 LAPP (商店"已装 → 运行"等只知道路径的入口): 现读包内 `need=wifi` 再决定。
 * 与 am_lua_open_if_entry 共用同一套门禁状态, 不会出现两条流程互相打架。 */
void am_lua_open_path(ui_ctx_t *ctx, const char *path) {
    apppack_meta_t m;
    bool need = (apppack_parse(path, &m) == ESP_OK) && m.need_wifi;
    wl_open(ctx, path, need);
}
/* LAPP 里的 icon= 是 64×64 位图 (打包器从主图标表取)。
 * ★★ 2026-10-15 "图标跟着应用走" (linit: 以后从服务器下发新应用不该为图标改固件):
 *   ① 先**反查**主图标表 —— 命中就用那个下标 (内置图标走原路, ABI 不变);
 *   ② 查不到 (服务器上新应用的自定义图标) → 注册进**运行时外来图标表**,
 *      返回 >= MAIN_ICON_COUNT 的新下标, 主菜单/管家/离屏列都能直接画;
 *   ③ 只有表满 (32 张) 才回退 ICON_MINI_GAMES。
 *   于是"上传新包到服务器 → 下载安装 → 图标正确显示"全链路不需要刷固件。 */
static int am_lua_icon_idx(const apppack_meta_t *m) {
    if (m->has_icon) {
        for (int i = 0; i < MAIN_ICON_COUNT; i++) {
            int w = 0, h = 0;
            const uint8_t *b = ui_main_icon_bitmap(i, &w, &h);
            if (b && w == MAIN_ICON_W && h == MAIN_ICON_H &&
                memcmp(b, m->icon, APPPACK_ICON_BYTES) == 0) return i;
        }
        int ex = ui_icon_register_extra(m->icon);
        if (ex >= 0) return ex;
    }
    return ICON_MINI_GAMES;
}
/* 分类 -> 英文目录: 只在"按分类过滤"里用到映射关系, 扫描已改成三目录一次扫完,
 * 所以这里保留成对照说明 (engine=引擎 / app=应用 / ops=运维)。 */
/* 分类 Lua 扫描 (后台任务执行, 写影子缓冲 s_lua_bg): 全局唯一函数, 扫指定分类目录
 * /store/apps 与 /sdcard/apps/{engine|app|ops}, 已扫分类(防重入)早退.
 * 结果暂存 s_lua_bg, 由 UI 线程 poll 迁移到 s_lua 并使用. */
/* ★ 2026-10-25: TF 卡首次使用/换新卡时自动建好三个分类目录, 用户不用手动建。
 *   /sdcard/apps/{engine|app|ops} —— 与 am_cat_folder() 的映射一致。
 *   幂等: 已存在时 mkdir 返回 EEXIST, 忽略即可。 */
/* ★★★ 2026-10-25 目录规范化: 旧布局 → 新布局 (按类型分二级目录)。
 *   rename 同分区瞬间完成, 不复制不耗时; 目标已存在则跳过(不覆盖)。
 *   老卡插上第一次进应用管家就自动搬好, 用户不用手动整理。 */
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
            /* 目标已存在: 不合并(风险), 只记一条日志让用户知道 */
            ESP_LOGW("AMLUA", "迁移跳过 %s (目标已存在)", M[i].old);
            continue;
        }
        if (rename(M[i].old, M[i].newp) == 0)
            ESP_LOGI("AMLUA", "迁移 %s → %s", M[i].old, M[i].newp);
        else
            ESP_LOGW("AMLUA", "迁移失败 %s", M[i].old);
    }
}

static void am_lua_ensure_dirs(void) {
    /* ★ 只需要第一次做: 每次切分类都跑 7 个 mkdir + 迁移检查, 在 TF 上是实打实的
     *   磁盘操作(每条约 5~30ms) ⇒ 曾让"切分类"白等几百毫秒。一次启动做一遍就够。 */
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

/* 一次扫完 engine/app/ops 三个目录 (每条记 fid), 之后切分类只做内存过滤。 */
static bool am_lua_scan(bool bg) {
    am_lua_ensure_dirs();
    if (s_lua_scanned) return true;                     /* 已扫过 → 数据就绪 */
    am_lua_entry_t *L = bg ? s_lua_bg : s_lua;          /* 后台走影子缓冲, 前台直写 */
    int *pn = bg ? &s_lua_bg_n : &s_lua_n;
    *pn = 0;
    static const char *const folders[3] = { "engine", "app", "ops" };
    const char *bases[2] = { "/store/apps", "/sdcard/apps" };
    for (int fi = 0; fi < 3; fi++) {
    const char *folder = folders[fi];
    for (int b = 0; b < 2; b++) {
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/%s", bases[b], folder);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            if (nm[0] == '.') continue;
            size_t len = strlen(nm);
            if (len <= 5 || strcasecmp(nm + len - 4, ".lua") != 0) continue;
            /* ★ 2026-10-09: **同名去重** (原来根本没有 —— 本文件开头的"合并去重"注释是错的)。
             *   两个 base 都扫, 同一应用在 SPIFFS(/store/apps) 与 TF(/sdcard/apps) 各放一份时,
             *   应用管家会出现**两条**: 一条叫 `map`, 另一条套了「」叫 `「map」`, 用户会当成
             *   两个应用 (而且 tF 那份常是旧版 ⇒ "有一个退不出来")。
             *   规则: /store/apps 优先 —— 它是应用商店的安装目标, 也是烧 apps_l.bin 的落点;
             *   /sdcard/apps 里同名的直接跳过 (TF 独有的应用仍然照常出现, 不受影响)。 */
            {
                bool dup = false;
                for (int j = 0; j < *pn; j++) {
                    const char *id = L[j].id;
                    if (strncmp(id, "lua:", 4) == 0 && strcmp(id + 4, nm) == 0) { dup = true; break; }
                }
                if (dup) continue;
            }
            if (*pn >= AM_LUA_MAX) break;
            int k = (*pn)++;
            L[k].fid = (uint8_t)fi;
            snprintf(L[k].id, sizeof(L[k].id), "lua:%s", nm);
            L[k].cat[0] = 0;
            { /* 显示名: LAPP 打包则用包内 name (可中文), 否则用文件名(去 .lua);
               * SD/TF 项加「」与内置区分 (LAPP 名同理加)。
               * ⚠ LAPP 的 cat= 也要读出来 —— 否则"分类"信息只能靠文件名白名单猜,
               *   新加一个 Lua 应用就得回来改 C 代码 (且子分类筛选会漏)。 */
              char pbuf[192];
              snprintf(pbuf, sizeof(pbuf), "%s/%s", dir, nm);
              apppack_meta_t meta;
              bool named = (apppack_parse(pbuf, &meta) == ESP_OK && meta.name[0]);
              if (named) {
                  snprintf(L[k].desc, sizeof(L[k].desc), "%s", meta.desc);
                  snprintf(L[k].cat,  sizeof(L[k].cat),  "%s", meta.cat);   /* 可空 */
              }
              L[k].need_wifi = named && meta.need_wifi;   /* 条目复用 → 必须显式赋值 */
              L[k].icon_idx = named ? am_lua_icon_idx(&meta) : ICON_MINI_GAMES;
              if (b == 1) {
            if (named) snprintf(L[k].label, sizeof(L[k].label), "\xe3\x80\x8c%.*s\xe3\x80\x8d", (int)sizeof(L[k].label) - 7, meta.name);
                  else       snprintf(L[k].label, sizeof(L[k].label), "\xe3\x80\x8c%.*s\xe3\x80\x8d", (int)len - 4, nm);
              } else {
                  if (named) snprintf(L[k].label, sizeof(L[k].label), "%s", meta.name);
                  else       snprintf(L[k].label, sizeof(L[k].label), "%.*s", (int)len - 4, nm);
              } }
            L[k].app.id        = L[k].id;
            L[k].app.label     = L[k].label;
            L[k].app.icon_idx  = L[k].icon_idx;         /* LAPP icon= 反查主图标表下表; 自定义位图回退小游戏机 */
            L[k].app.page      = OS_PAGE_LUA;
            L[k].app.hidden    = true;
            L[k].app.touch_only= false;
            L[k].app.cat       = L[k].cat[0] ? L[k].cat : NULL;   /* 交给 LAPP 元信息决定分类 */
            L[k].app.reveal    = false;
            snprintf(L[k].path, sizeof(L[k].path), "%s/%s", dir, nm);
        }
        closedir(d);
    }
    }   /* for fi (engine/app/ops) */
    if (!bg) s_lua_scanned = true;        /* 前台直写完成才标记已扫 */
    return true;
}
/* 后台扫描任务: 一次扫完三个目录, 由 UI poll 取货. */
static void am_lua_scan_task(void *arg) {
    (void)arg;
    am_lua_scan(true);
    s_lua_done = true;
    s_lua_busy = false;
    vTaskDelete(NULL);
}
/* 启动后台扫描 (幂等): 已扫/扫描中早退; 保证前台 rebuild 永不被 SPIFFS 阻塞. */
static void am_lua_scan_async(void) {
    if (s_lua_scanned || s_lua_busy) return;
    s_lua_busy = true;
    s_lua_done = false;
    if (xTaskCreatePinnedToCore(am_lua_scan_task, "am_luascan", 8192, NULL, 3, NULL, 0) != pdPASS) {
        s_lua_busy = false;
        s_lua_done = false;
        ESP_LOGD(TAG, "Lua 扫描任务创建失败(内存不足?), 允许重试");
    }
}

/* 该 Lua 文件是否属于运维类。
 * 判据: ① LAPP 元信息 cat=="运维" (权威, 加新的运维应用不用改 C 代码)
 *       ② 回退: 文件名白名单 (兼容未打包 / 老的 .lua) */
static bool am_lua_is_ops(const am_lua_entry_t *e) {
    static const char *const s_ops_lua[] = {
        "netdect.lua", "wifiprobe.lua", "logdiag.lua",
        "winassist.lua", "diag.lua", "term.lua"
    };
    if (e->cat[0] && strcmp(e->cat, "\xe8\xbf\x90\xe7\xbb\xb4") == 0) return true;   /* 运维 */
    const char *fname = e->id + 4;                    /* id = "lua:xxx.lua" */
    for (int i = 0; i < (int)(sizeof(s_ops_lua) / sizeof(s_ops_lua[0])); i++)
        if (strcmp(fname, s_ops_lua[i]) == 0) return true;
    return false;
}
/* 追加当前分类的 Lua 文件项到 s_list.
 * ops_only=true  (运维分类): 只加运维类 Lua;
 * ops_only=false (引擎/应用) : 排除运维类; 且**只在"应用"分类**按顶部子分类过滤 cat
 *   —— 原先这里不过滤, 于是"计算器/白板"在 游戏/壁纸/学习 每个子分类里都会出现。
 *   ⚠ 不能对所有分类都过滤: s_sub 切左侧分类时**不会复位**, 在"应用→游戏"后切到"引擎",
 *     残留的 s_sub=游戏 会把引擎类 Lua 文件全滤掉 (引擎分类本来也没有子分类条)。 */
static void am_append_lua(bool ops_only) {
    /* 目录归属过滤: 引擎/应用/运维 各只显示自己目录里的 Lua。
     * (原来靠"逐分类重扫"天然隔离, 现在一份数据共享, 必须显式按 fid 过滤) */
    const int want = (am_cat() == AM_CAT_ENGINE) ? 0 : (am_cat() == AM_CAT_OPS) ? 2 : 1;
    for (int i = 0; i < s_lua_n; i++) {   /* 上限由 am_push_app 判定 */
        am_lua_entry_t *e = &s_lua[i];
        if ((int)e->fid != want) continue;
        if (am_lua_is_ops(e) != ops_only) continue;
        if (!ops_only && am_cat() == AM_CAT_APP && !am_sub_want_cat(e->app.cat)) continue;
        am_push_app(&e->app);
    }
}


/* ==================== 对外接口 (声明见 am_internal.h) ==================== */
/* 切分类时调用: 已扫过 → 立刻可用; 未扫 → 启动后台扫描 (幂等)。
 * ★ 现在数据与分类无关(一份全目录数据), 所以切分类不用再等扫描。 */
void am_lua_prefetch(int cat) {
    (void)cat;
    if (!s_lua_scanned) am_lua_scan_async();
}
bool am_lua_ready(int cat) { (void)cat; return s_lua_scanned; }
void am_lua_append(bool ops_only) { am_append_lua(ops_only); }
void am_lua_invalidate(void) { s_lua_scanned = false; }
/* 每帧取货: 影子缓冲搬进正式缓冲并标记就绪; true = 数据有更新, 调用方应重建列表 */
bool am_lua_tick(void) {
    if (!s_lua_done) return false;
    s_lua_done = false;
    s_lua_n = s_lua_bg_n;
    if (s_lua_n > 0) memcpy(s_lua, s_lua_bg, sizeof(am_lua_entry_t) * (size_t)s_lua_n);
    s_lua_scanned = true;
    return true;
}
