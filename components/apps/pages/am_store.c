/* ============================================================
 * am_store.c — 应用中心 · 商店模块
 * (2026-09-20 从 page_app_manager.c 拆出, 原单文件 1648 行)
 *
 * 职责: 扫描本地应用包(SPIFFS /store 根 + TF /sdcard/store_pkg 的 .lua);
 *       "安装" = 复制到分类目录(/store/apps/app 或 /sdcard/apps/app), 卸载 = 删除副本。
 * 扫描与复制都在**后台任务**里执行, 不阻塞 UI 首帧; UI 通过 am_store_tick() 取货。
 * 与页面主逻辑只经 am_internal.h 交互。
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>   /* mkdir */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "am_internal.h"
#include "apppack.h"   /* LAPP 单文件资源包: 取名称/简介 */

#define TAG "AMSTORE"

/* ========== 商店 本地应用包 (不联网) ==========
 * 商店不联网: 扫描 /store(SPIFFS根, 预置包) 与 /sdcard/store_pkg 下的 .lua 新版本应用包
 * (用户把已制作好的 .lua 放进去). 按中文名称展示; 点击 → 复制到对应分类目录
 * /store/apps/{engine|game|app}/{name}.lua, 分类扫描后即可运行.
 * 原固件 C 内置应用不再进商店. */
#define AM_STORE_SRC1   "/store"              /* SPIFFS 根: merge 脚本把 apps 的 lua 全部打包进根目录 */
#define AM_STORE_SRC2   "/sdcard/store_pkg"
#define AM_STORE_DST    "/store/apps"
#define AM_PKG_MAX      48

typedef struct {
    os_app_t app;
    char     id[48];     /* pkg:xxx.lua */
    char     label[48];  /* 中文名 */
    char     cat[16];    /* 分类文件夹 engine/game/app */
    char     src[192];   /* 源完整路径 */
    char     dst[192];   /* 目标完整路径(内置固件) */
    char     dst_sd[192];/* 目标完整路径(TF卡) */
    int      pref;       /* 推荐安装目标: 0=内置固件 1=TF卡 */
    char     fname[48];  /* 文件名 xxx.lua */
    char     desc[96];   /* LAPP 简介 (无打包则空) */
} am_pkg_entry_t;
static EXT_RAM_BSS_ATTR am_pkg_entry_t s_pkg[AM_PKG_MAX];
static int s_pkg_n = 0;
static volatile int s_pkg_scanned = -1;   /* 0=已扫描 (数据可用)  -1=未扫/扫描中 (后台任务写入) */
static volatile bool s_pkg_busy  = false;   /* 后台扫描任务进行中 */
static volatile bool s_pkg_done  = false;   /* 后台扫描已完成 (UI 线程取货) */
static bool s_pkg_inst[AM_PKG_MAX]; /* 每包安装状态缓存: 渲染用, 避免每帧 fopen(SD/SPIFFS 慢)致商店卡顿 */

/* 商店包元数据表已移除 (2026-09-19): 原 10 个预置包(Lua 单文件版)已弃用删除。
 * 商店改为"扫到什么列什么", 显示名直接用文件名(以后要中文名可在包内 AppInfo 里带)。 */
static bool am_file_exist(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}
/* 安装目标默认值: 0=内置固件 /store/apps, 1=TF 卡 /sdcard/apps。
 * (原"运维类包推荐装 TF 卡"的按名规则随预置包一并移除; 以后需要可按包名再扩展) */
static int am_pkg_prefer(const char *fname) { (void)fname; return 0; }
static bool am_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return false; }
    fclose(in); fclose(out);
    return true;
}
static int am_pkg_find_name(const char *fname) {
    for (int i = 0; i < s_pkg_n; i++)
        if (strcmp(s_pkg[i].fname, fname) == 0) return i;
    return -1;
}
/* 扫描商店本地包 (先内部再 SD, 同名校则覆盖 => sdcard 优先), 仅一次.
 * 全程在后台任务执行 (SPIFFS opendir + 逐包 2×fopen 较慢, 不能阻塞首帧);
 * s_pkg_scanned 在扫描完成时才置 0, 期间 UI 显示"正在扫描…". */
static void am_pkg_scan(void) {
    /* 只按 s_pkg_scanned 防重入 (已扫描则不再扫). 不查 s_pkg_busy:
     * 后台任务启动前已置 s_pkg_busy=true, 若在此再判 busy 会把自己挡退 → 扫描永不执行
     * ("一直正在扫描"根因). */
    if (s_pkg_scanned == 0) return;
    s_pkg_n = 0;
    const char *bases[2] = { AM_STORE_SRC1, AM_STORE_SRC2 };
    for (int b = 0; b < 2; b++) {
        DIR *d = opendir(bases[b]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            if (nm[0] == '.') continue;
            size_t len = strlen(nm);
            if (len <= 5 || strcasecmp(nm + len - 4, ".lua") != 0) continue;
            int k = am_pkg_find_name(nm);
            if (k < 0) {
                if (s_pkg_n >= AM_PKG_MAX) break;
                k = s_pkg_n++;
                snprintf(s_pkg[k].fname, sizeof(s_pkg[k].fname), "%s", nm);
                snprintf(s_pkg[k].id, sizeof(s_pkg[k].id), "pkg:%s", nm);
                /* 无预置元数据表: 显示名取文件名(去 .lua), 分类默认 app。
                 * 经局部缓冲中转 —— 直接 snprintf(s_pkg…, "%.*s", nm) 会触发
                 * -Werror=restrict (编译器无法排除 nm 与 s_pkg 重叠)。 */
                char nb[64];
                snprintf(nb, sizeof(nb), "%.*s", (int)len - 4, nm);
                /* LAPP 打包则用包内 name/desc (可中文), 否则文件名 */
                char spath[192];
                snprintf(spath, sizeof(spath), "%s/%s", bases[b], nm);
                apppack_meta_t meta;
                if (apppack_parse(spath, &meta) == ESP_OK && meta.name[0]) {
                    snprintf(s_pkg[k].label, sizeof(s_pkg[k].label), "%s", meta.name);
                    snprintf(s_pkg[k].desc, sizeof(s_pkg[k].desc), "%s", meta.desc);
                } else {
                    snprintf(s_pkg[k].label, sizeof(s_pkg[k].label), "%s", nb);
                }
                snprintf(s_pkg[k].cat, sizeof(s_pkg[k].cat), "app");
                snprintf(s_pkg[k].dst, sizeof(s_pkg[k].dst), "%s/app/%s", AM_STORE_DST, nm);
                snprintf(s_pkg[k].dst_sd, sizeof(s_pkg[k].dst_sd), "/sdcard/apps/app/%s", nm);
                s_pkg[k].pref = am_pkg_prefer(nm);
                s_pkg[k].app.id        = s_pkg[k].id;
                s_pkg[k].app.label     = s_pkg[k].label;
                s_pkg[k].app.icon_idx  = ICON_MINI_GAMES;
                s_pkg[k].app.page      = OS_PAGE_LUA;
                s_pkg[k].app.hidden    = true;
                s_pkg[k].app.touch_only= false;
                s_pkg[k].app.cat       = NULL;
                s_pkg[k].app.reveal    = false;
            }
            snprintf(s_pkg[k].src, sizeof(s_pkg[k].src), "%s/%s", bases[b], nm);
            /* 缓存安装状态: 仅扫描时算一次, 渲染不再逐帧 fopen */
            s_pkg_inst[k] = am_file_exist(s_pkg[k].dst) || am_file_exist(s_pkg[k].dst_sd);
        }
        closedir(d);
    }
    s_pkg_scanned = 0;   /* 全部填充完成, UI 才可见数据 */
}
/* 后台扫描任务: 商店包扫描 (SPIFFS/SD 慢, 不阻塞 UI 首帧) */
static void am_pkg_scan_task(void *arg) {
    (void)arg;
    am_pkg_scan();
    s_pkg_done = true;   /* 通知 UI 线程取货 (重建列表) */
    s_pkg_busy = false;
    vTaskDelete(NULL);
}
void am_store_prefetch(void) {
    if (s_pkg_scanned == 0 || s_pkg_busy) return;
    s_pkg_busy = true;
    s_pkg_done = false;
    /* V1.2 修复: 原实现忽略返回值 —— 任务创建失败(内存不足)时 s_pkg_busy 永久为 true,
     * 商店会一直显示"正在扫描…"且无法重试 (prefetch 被 busy 挡退). 失败须回滚标志. */
    if (xTaskCreatePinnedToCore(am_pkg_scan_task, "pkgscan", 8192, NULL, 3, NULL, 0) != pdPASS) {
        s_pkg_busy = false;
        s_pkg_done = false;
        ESP_LOGE(TAG, "商店扫描任务创建失败(内存不足?), 保留上次结果并允许重试");
    }
}
/* 商店源清理 (V1.6.x): 商店"本地源" = /store 根目录的 .lua 待装包; "已安装"只认
 * 内置分类目录 /store/apps/{cat} 的正式副本 (模拟"点击安装→复制到分类目录").
 * 每次扫描完成都删掉 SD 卡 /sdcard/apps/{cat} 的同名"拆封独立"副本 (用户已放弃那套,
 * 只保留商店源, 防止旧副本让商店误显示为"已安装"); remove 幂等 (不存在无害), 只限
 * 商店源同名文件, 不影响用户放到 SD 的其它程序. */
/* 商店扫描完成后的状态刷新。
 * ⚠ 这里**不能删 SD 卡副本**: 安装目标是可选的(pref=1 的包默认装 /sdcard/apps),
 *  旧实现无条件 remove(dst_sd) + 清 lua 主菜单记录, 会把刚"装到 TF 卡"的包当场删掉、
 *  商店随即又显示"未安装" → "安装到 TF 卡"功能整体失效。
 * 只做一件事: 重算每包的安装状态缓存(渲染不再逐帧 fopen)。 */
static void am_pkg_cleanup(void) {
    for (int k = 0; k < s_pkg_n; k++)
        s_pkg_inst[k] = am_file_exist(s_pkg[k].dst) || am_file_exist(s_pkg[k].dst_sd);
    am_lua_invalidate();   /* Lua 列表下次重扫 */
    ESP_LOGI(TAG, "商店包状态已刷新 (内置 /store/apps 或 TF 卡 任一存在即视为已安装)");
}
static int am_pkg_find(const os_app_t *a) {
    for (int i = 0; i < s_pkg_n; i++)
        if (&s_pkg[i].app == a) return i;
    return -1;
}
static bool am_pkg_installed(int i) {
    if (i < 0 || i >= s_pkg_n) return false;
    /* 内置(/store/apps) 或 TF 卡(/sdcard/apps) 任一存在即算已安装 —— 必须与
     * 扫描时写入的 s_pkg_inst[] 判定一致, 否则列表显示"已装"、点击却弹"安装 XXX?"。 */
    return am_file_exist(s_pkg[i].dst) || am_file_exist(s_pkg[i].dst_sd);
}
/* 安装/卸载后刷新单包安装状态缓存 (只能算一次, 不要在渲染里 fopen) */
static void am_reload_pkg_inst(int k) {
    if (k < 0 || k >= s_pkg_n) return;
    s_pkg_inst[k] = am_file_exist(s_pkg[k].dst) || am_file_exist(s_pkg[k].dst_sd);
}
/* 安装目标选择回调: result 0=内置固件 1=TF卡 */
/* 确认安装回调 (V1.6.x): 安装目标不再让用户选择, 由应用自身默认 s_pkg[k].pref 决定
 * (结果 0=确认安装; 1=取消). 现有应用默认装内置; 以后大的游戏在对应应用设默认装 TF. */
static void am_store_install_cb(ui_ctx_t *ctx, int result, void *ud) {
    int k = (int)(long)ud;
    if (k < 0 || k >= s_pkg_n) return;
    if (result != 0) { am_rebuild_keep(); return; }  /* 取消: 弹窗由框架自动关 */
    int to_tf = s_pkg[k].pref;                       /* 应用自身默认: 1=TF卡 0=内置固件 */
    const char *base = to_tf ? "/sdcard/apps" : AM_STORE_DST;
    char catdir[64];
    snprintf(catdir, sizeof(catdir), "%s/%s", base, s_pkg[k].cat);
    mkdir(base, 0755);
    mkdir(catdir, 0755);
    const char *dest = to_tf ? s_pkg[k].dst_sd : s_pkg[k].dst;
    if (am_copy_file(s_pkg[k].src, dest)) {
        os_dialog_toast(ctx, to_tf ? "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85\xe5\x88\xb0TF\xe5\x8d\xa1"   /* 已安装到TF卡 */
                                   : "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85\xe5\x88\xb0\xe5\x86\x85\xe7\xbd\xae"); /* 已安装到内置 */
        am_lua_invalidate();   /* Lua 列表下次重扫 */
        am_reload_pkg_inst(k);                       /* 安装后只刷新该包缓存一次 */
    } else {
        os_dialog_toast(ctx, "\xe5\xae\x89\xe8\xa3\x85\xe5\xa4\xb1\xe8\xb4\xa5");   /* 安装失败 */
    }
    am_rebuild_keep();
}
/* 商店选中: 已安装(内置或卡) → 运行; 未安装 → 弹"安装 XXX?" 确认 (目标是应用默认, 不选) */
void am_store_activate(ui_ctx_t *ctx, const os_app_t *a) {
    int k = am_pkg_find(a);
    if (k < 0) return;
    if (am_pkg_installed(k)) {
        const char *run = am_file_exist(s_pkg[k].dst) ? s_pkg[k].dst : s_pkg[k].dst_sd; /* 优先内置, 否则TF卡 */
        os_page_lua_set_boot(run);
        os_push(ctx, OS_PAGE_LUA);
        return;
    }
    char title[64];
    snprintf(title, sizeof(title), "\xe5\xae\x89\xe8\xa3\x85 %s ?", s_pkg[k].label); /* 安装 XXX ? */
    os_dialog_confirm_ex(ctx, title, 0, 0, am_store_install_cb, (void *)(long)k);
}

/* ========== 商店 卸载 (删除已装包 + 移出主菜单 + 重新扫描) ========== */
static void am_store_uninstall_do(ui_ctx_t *ctx, int k) {
    if (k < 0 || k >= s_pkg_n) return;
    if (!am_pkg_installed(k)) { os_dialog_toast(ctx, "\xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85"); return; } /* 未安装 */
    remove(s_pkg[k].dst);                                  /* 删掉 /store/apps/{cat}/{file} */
    char sd[192];
    snprintf(sd, sizeof(sd), "/sdcard/apps/%s/%s", s_pkg[k].cat, s_pkg[k].fname); /* 联动删 SD 卡同名副本 */
    if (am_file_exist(sd)) remove(sd);
    char lid[64];
    snprintf(lid, sizeof(lid), "lua:%s", s_pkg[k].fname);  /* 同步清掉对应 Lua 项的主菜单记录 */
    os_app_set_mainmenu(lid, false);
    os_app_set_hidden(lid, true);
    am_lua_invalidate();   /* Lua 列表下次重扫 */
    os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x8d\xb8\xe8\xbd\xbd");   /* 已卸载 */
    am_reload_pkg_inst(k);                       /* 卸载后只刷新该包缓存一次 */
    am_rebuild_keep();
    ctx->needs_redraw = true;
}
static void am_store_uninstall_cb(ui_ctx_t *ctx, int result, void *ud) {
    if (result == 0) am_store_uninstall_do(ctx, (int)(long)ud);
    os_dialog_pop(ctx);                                    /* 关掉确认框 (toast 会置 handled, 这里补关) */
    ctx->needs_redraw = true;
}
/* 选中的商店项若已安装 → 弹"卸载 XX?" 确认 */
void am_store_uninstall_ask(ui_ctx_t *ctx, const os_app_t *a) {
    if (!a) return;
    int k = am_pkg_find(a);
    if (k < 0 || !am_pkg_installed(k)) return;
    char msg[64];
    snprintf(msg, sizeof(msg), "\xe5\x8d\xb8\xe8\xbd\xbd %s?", s_pkg[k].label); /* 卸载 XX? */
    os_dialog_confirm_ex(ctx, msg, 0, 0, am_store_uninstall_cb, (void *)(long)k);
}

/* ==================== 对外接口 (声明见 am_internal.h) ==================== */
bool am_store_ready(void) { return s_pkg_scanned == 0; }
int  am_store_count(void) { return (s_pkg_scanned == 0) ? s_pkg_n : 0; }

const os_app_t *am_store_app(int k) {
    return (k >= 0 && k < s_pkg_n) ? &s_pkg[k].app : NULL;
}
bool am_store_owns(const os_app_t *a) { return am_pkg_find(a) >= 0; }
bool am_store_installed_of(const os_app_t *a) {
    int k = am_pkg_find(a);
    return (k >= 0) && s_pkg_inst[k];   /* 读缓存: 渲染路径不能逐帧 fopen */
}
const char *am_store_fname_of(const os_app_t *a) {
    int k = am_pkg_find(a);
    return (k >= 0) ? s_pkg[k].fname : NULL;
}
/* 目录变化(装入/卸载)后调用: 下次进商店重新扫描 */
void am_store_invalidate(void) { s_pkg_scanned = -1; s_pkg_busy = false; s_pkg_done = false; }

/* 每帧取货: 后台扫描完成后刷新"安装状态"缓存; 返回 true = UI 应重建列表 */
bool am_store_tick(void) {
    if (!s_pkg_done) return false;
    s_pkg_done = false;
    am_pkg_cleanup();
    return true;
}
