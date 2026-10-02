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
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "am_internal.h"
#include "apppack.h"   /* LAPP 单文件资源包: 取名称/简介/分类/图标 */
#include "ui_common.h" /* ui_main_icon_bitmap: LAPP icon= 反查主图标表下标 */

#define TAG "AMLUA"

/* 扫描状态 (原声明在商店块, 拆分后归本模块) */
static int s_lua_scan_cat = -1;   /* 已扫描(或正在扫)的分类 */
static int s_lua_ok_cat   = -1;   /* 当前有效数据对应的分类 */

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
static int            s_lua_scan_task_cat = -1; /* 本次后台扫描的分类 (任务线程设置) */

/* 该应用指针是否为 Lua 文件项; 是则回填完整路径 */
bool am_lua_is_entry(const os_app_t *a, const char **path_out) {
    for (int i = 0; i < s_lua_n; i++)
        if (&s_lua[i].app == a) { if (path_out) *path_out = s_lua[i].path; return true; }
    return false;
}
/* 启动 Lua 文件项 (应用管家与主菜单共用): 先设 boot 路径再进 Lua 页。
 * 主菜单直接 os_push(OS_PAGE_LUA) 会拿不到路径 → 打开空白/上次的程序, 所以必须走这里。 */
bool am_lua_open_if_entry(ui_ctx_t *ctx, const os_app_t *a) {
    const char *p = NULL;
    lua_probe_stage(1, "主菜单 open_if_entry");
    if (!am_lua_is_entry(a, &p) || !p) return false;
    lua_probe_stage(2, "Lua 项命中(主菜单)");
    os_page_lua_set_boot(p);
    os_push(ctx, OS_PAGE_LUA);
    return true;
}
/* LAPP 里的 icon= 是 64×64 位图 (打包器从主图标表取)。os_app_t 只有 icon_idx,
 * 没有"自带位图"字段 → 这里做**反查**: 逐张比对主图标表, 命中就用那个下标,
 * 于是 `pack_app.py --icon-from-main ICON_XXX` 打的图标在管家/主菜单能正确显示。
 * 自定义位图 (TF 侧手绘) 匹配不上 → 回退 ICON_MINI_GAMES (小游戏机图标)。 */
static int am_lua_icon_idx(const apppack_meta_t *m) {
    if (m->has_icon) {
        for (int i = 0; i < MAIN_ICON_COUNT; i++) {
            int w = 0, h = 0;
            const uint8_t *b = ui_main_icon_bitmap(i, &w, &h);
            if (b && w == MAIN_ICON_W && h == MAIN_ICON_H &&
                memcmp(b, m->icon, APPPACK_ICON_BYTES) == 0) return i;
        }
    }
    return ICON_MINI_GAMES;
}
/* 分类 -> 英文目录 (引擎/游戏/应用) */
static const char *am_cat_folder(int cat) {
    switch (cat) {
    case AM_CAT_ENGINE: return "engine";
    case AM_CAT_OPS:    return "ops";
    default:            return "app";
    }
}
/* 分类 Lua 扫描 (后台任务执行, 写影子缓冲 s_lua_bg): 全局唯一函数, 扫指定分类目录
 * /store/apps 与 /sdcard/apps/{engine|app|ops}, 已扫分类(防重入)早退.
 * 结果暂存 s_lua_bg, 由 UI 线程 poll 迁移到 s_lua 并使用. */
static bool am_lua_scan(int cat, bool bg) {
    if (s_lua_scan_cat == cat) return true;             /* 已扫过 → 数据就绪 */
    am_lua_entry_t *L = bg ? s_lua_bg : s_lua;          /* 后台走影子缓冲, 前台直写 */
    int *pn = bg ? &s_lua_bg_n : &s_lua_n;
    *pn = 0;
    const char *folder = am_cat_folder(cat);
    const char *bases[2] = { "/store/apps", "/sdcard/apps" };
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
            if (*pn >= AM_LUA_MAX) break;
            int k = (*pn)++;
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
    if (!bg) s_lua_scan_cat = cat;        /* 前台直写完成才标记已扫 */
    return true;
}
/* 分类 Lua 后台扫描任务: 扫当前分类直到标记 done, 由 UI poll 取货. */
static void am_lua_scan_task(void *arg) {
    int cat = (int)(long)arg;
    s_lua_scan_task_cat = cat;
    am_lua_scan(cat, true);
    s_lua_done = true;
    s_lua_busy = false;
    vTaskDelete(NULL);
}
/* 启动分类 Lua 后台扫描 (幂等): 已扫/扫描中早退; 保证前台 rebuild 永不被 SPIFFS 阻塞.
 * 不查 s_lua_done: done 表示后台已完成等待 UI 取货, 与"启动新扫描"无关 (取货后置 false). */
static void am_lua_scan_async(int cat) {
    if (s_lua_scan_cat == cat || s_lua_busy) return;
    s_lua_busy = true;
    s_lua_done = false;
    /* V1.2 修复: 与 am_store_prefetch 同 —— 任务创建失败必须回滚 busy,
     * 否则该分类永久卡在"正在扫描…"且无法重试. */
    if (xTaskCreatePinnedToCore(am_lua_scan_task, "am_luascan", 8192, (void *)(long)cat, 3, NULL, 0) != pdPASS) {
        s_lua_busy = false;
        s_lua_done = false;
        ESP_LOGE(TAG, "Lua 扫描任务创建失败(内存不足?), 允许重试");
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
    for (int i = 0; i < s_lua_n; i++) {   /* 上限由 am_push_app 判定 */
        am_lua_entry_t *e = &s_lua[i];
        if (am_lua_is_ops(e) != ops_only) continue;
        if (!ops_only && am_cat() == AM_CAT_APP && !am_sub_want_cat(e->app.cat)) continue;
        am_push_app(&e->app);
    }
}


/* ==================== 对外接口 (声明见 am_internal.h) ==================== */
/* 切分类时调用: 已扫过 → 标记就绪; 未扫 → 启动后台扫描 (幂等) */
void am_lua_prefetch(int cat) {
    if (s_lua_scan_cat == cat) { s_lua_ok_cat = cat; return; }
    am_lua_scan_async(cat);
    s_lua_ok_cat = -1;
}
bool am_lua_ready(int cat) { return s_lua_ok_cat == cat; }
void am_lua_append(bool ops_only) { am_append_lua(ops_only); }
void am_lua_invalidate(void) { s_lua_scan_cat = -1; s_lua_ok_cat = -1; }
/* 每帧取货: 影子缓冲搬进正式缓冲并标记就绪; true = 数据有更新, 调用方应重建列表 */
bool am_lua_tick(void) {
    if (!s_lua_done) return false;
    s_lua_done = false;
    s_lua_n = s_lua_bg_n;
    if (s_lua_n > 0) memcpy(s_lua, s_lua_bg, sizeof(am_lua_entry_t) * (size_t)s_lua_n);
    s_lua_scan_cat = s_lua_scan_task_cat;
    s_lua_ok_cat   = s_lua_scan_cat;
    return true;
}
