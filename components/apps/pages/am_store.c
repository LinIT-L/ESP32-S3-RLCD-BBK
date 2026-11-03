/**
 * am_store.c — 应用商店：**动作层**
 *
 * ════════════════════════════════════════════════════════════════════════
 * 2026-11-03 起的分工（改这个文件前先读这三行）
 * ------------------------------------------------------------------------
 *   应用（Lua / LPK / 内置 C）的**发现、身份、安装状态**全部来自 `am_app_core`。
 *   本文件不再扫描任何应用目录、不再维护"某个应用装没装"的第二套判断，
 *   只做四件事：列表投影、确认框、下载、安装/更新/卸载。
 *
 *   唯一留在本文件的数据是**资源包**（index.json 里 type=asset / type=lpk）：
 *   它们没有 app_id、没有运行入口，装完只是往 target 目录铺文件给别的应用当素材，
 *   既不是应用也不能启动 ⇒ 不参与应用身份/去重，单独一张小表。
 *   ⚠ 曾经把 type=lpk 当成"LPK 应用"，于是《仙剑奇侠传》的资源包被渲染成一个
 *     点开必然失败的应用条目 —— 那是把"资源"和"应用"混为一谈的直接后果。
 * ════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"

#include "am_internal.h"
#include "am_app_core.h"
#include "am_lpkg.h"
#include "am_asset.h"
#include "am_store_state.h"
#include "font_book.h"
#include "wallpapers.h"
#include "svc_cfg.h"
#include "wifi_manager.h"
#include "sd_scan.h"

#define TAG "AMSTORE"

#define D_TF_STORE_PKG "/sdcard/store_pkg"
#define D_FLASH_ROOT   "/store"

/* ════════════════════════════════════════════════════════════════════════
 * 资源包（只存在于商店，从不进应用中心）
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct {
    os_app_t app;
    char     id[64];
    char     label[64];
    char     cat[16];
    char     zip[96];      /* 索引里的 path，如 pkgs/font_wqy16.zip */
    char     target[96];   /* 解包目录 */
    int      ridx;
    bool     installed;    /* **缓存**的安装状态：渲染路径绝不能每帧去 stat/读清单 */
} am_res_row_t;

static EXT_RAM_BSS_ATTR am_res_row_t s_res[AM_RPKG_MAX];
static int      s_res_n = 0;
static uint32_t s_res_gen_done = 0;
static bool     s_res_dirty = true;

static int cat_icon(const char *cat)
{
    if (!cat || !cat[0])          return ICON_APP_MGR;
    if (strcmp(cat, "游戏") == 0) return ICON_MINI_GAMES;
    if (strcmp(cat, "运维") == 0) return ICON_TERMINAL;
    if (strcmp(cat, "标准") == 0) return ICON_SETTINGS;
    if (strcmp(cat, "学习") == 0) return ICON_BOOK;
    if (strstr(cat, "壁纸") || strcmp(cat, "资源") == 0 ||
        strcmp(cat, "wallpaper") == 0) return ICON_WALLPAPER;
    return ICON_APP_MGR;
}

static bool am_is_lpk_path(const char *p)
{
    if (!p) return false;
    size_t n = strlen(p);
    return (n > 4) && (strcasecmp(p + n - 4, ".lpk") == 0);
}

/* 资源包是否"确实装好"：按 (解包目录, zip 路径) 判定，全工程只此一处。
 *   .lpk 走 registry；zip 走 .installed.<stem> + .pkg.<stem>.lst 清单回验。 */
static bool am_asset_installed_at(const char *dir, const char *zip)
{
    if (!zip || !zip[0]) return false;
    if (am_is_lpk_path(zip)) {
        char id[64];
        am_lpkg_stem_of(zip, id, sizeof(id));
        if (id[0] && am_lpkg_is_installed(id)) return true;
        if (!dir || !dir[0]) return false;
    } else if (!dir || !dir[0]) {
        return false;
    }

    char stem[64] = {0};
    {
        const char *b = strrchr(zip, '/');
        b = b ? b + 1 : zip;
        size_t k = 0;
        while (b[k] && b[k] != '.' && k + 1 < sizeof(stem)) { stem[k] = b[k]; k++; }
        stem[k] = 0;
    }
    if (!stem[0]) return false;

    char mark[176], lst[176];
    snprintf(mark, sizeof(mark), "%s/.installed.%s", dir, stem);
    snprintf(lst,  sizeof(lst),  "%s/.pkg.%s.lst", dir, stem);

    int  want_cnt = -1;
    unsigned long long want_bytes = 0;
    bool has_bytes = false;
    FILE *mf = fopen(mark, "r");
    if (mf) {
        int got = fscanf(mf, "%d %llu", &want_cnt, &want_bytes);
        has_bytes = (got == 2);
        fclose(mf);
    } else {
        char om[128];
        snprintf(om, sizeof(om), "%s/.installed", dir);
        FILE *of = fopen(om, "r");
        if (!of) return false;
        fclose(of);
    }

    FILE *lf = fopen(lst, "r");
    if (!lf) return false;
    int  cnt = 0;
    unsigned long long bytes = 0;
    char line[288];
    while (fgets(line, sizeof(line), lf)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (line[0] == '/' || strstr(line, "..")) { fclose(lf); return false; }
        char fp[336];
        snprintf(fp, sizeof(fp), "%s/%s", dir, line);
        struct stat st;
        if (stat(fp, &st) != 0 || st.st_size <= 0) { fclose(lf); return false; }
        const char *dot = strrchr(line, '.');
        if (dot && (strcasecmp(dot, ".lbf") == 0)) {
            FILE *gf = fopen(fp, "rb");
            if (!gf) { fclose(lf); return false; }
            char mg[4] = {0};
            bool okm = (fread(mg, 1, 4, gf) == 4 && memcmp(mg, "LBF1", 4) == 0);
            fclose(gf);
            if (!okm) { fclose(lf); return false; }
        }
        bytes += (unsigned long long)st.st_size;
        cnt++;
    }
    fclose(lf);
    if (cnt <= 0) return false;
    if (want_cnt >= 0 && cnt != want_cnt) return false;
    if (has_bytes && bytes != want_bytes) return false;
    return true;
}

/* 从服务器索引重建资源包行。索引代次没变就不重建。 */
static void res_sync(void)
{
    uint32_t g = am_rstore_gen();
    if (!s_res_dirty && g == s_res_gen_done) return;
    s_res_dirty = false;
    s_res_gen_done = g;
    s_res_n = 0;
    for (int i = 0; i < am_rstore_count() && s_res_n < AM_RPKG_MAX; i++) {
        const am_rpkg_t *rp = am_rstore_get(i);
        if (!rp || !rp->name[0] || !rp->path[0]) continue;
        if (!am_pkg_is_res(rp->type)) continue;      /* 应用条目归统一核心 */
        am_res_row_t *r = &s_res[s_res_n++];
        memset(r, 0, sizeof(*r));
        r->ridx = i;
        snprintf(r->id, sizeof(r->id), "res:%s", rp->name);
        snprintf(r->label, sizeof(r->label), "%s", rp->label[0] ? rp->label : rp->name);
        snprintf(r->cat, sizeof(r->cat), "%s", rp->cat);
        snprintf(r->zip, sizeof(r->zip), "%s", rp->path);
        snprintf(r->target, sizeof(r->target), "%s", rp->target);
        r->app.id       = r->id;
        r->app.label    = r->label;
        r->app.cat      = r->cat[0] ? r->cat : NULL;
        r->app.icon_idx = cat_icon(r->cat);
        r->app.page     = OS_PAGE_LUA;
        r->app.hidden   = true;
        /* 安装状态在这里算一次并缓存 —— 渲染/点击都读这个字段。
         * 它是"清单回验"，涉及 fopen/stat，逐帧调用会明显拖慢商店。 */
        r->installed = am_asset_installed_at(r->target, r->zip);
    }
}

static am_res_row_t *res_find(const os_app_t *a)
{
    if (!a || !a->id) return NULL;
    for (int i = 0; i < s_res_n; i++) if (s_res[i].app.id == a->id) return &s_res[i];
    return NULL;
}

static const char *res_stem(const am_res_row_t *r)
{
    const char *b = strrchr(r->zip, '/');
    return b ? b + 1 : r->zip;
}

/* ════════════════════════════════════════════════════════════════════════
 * 忙碌登记（下载由 net 模块的全局单例负责，卸载在本模块后台任务里跑）
 * ════════════════════════════════════════════════════════════════════════ */

static char          s_uninst_key[AM_CORE_KEY_LEN];
static char          s_uninst_rid[AM_CORE_KEY_LEN];
static volatile bool s_uninst_busy = false;
static volatile bool s_uninst_done = false;

static void dlg_title16(ui_ctx_t *ctx)
{
    os_dlg_stack_t *d = os_dialog_top_mut(ctx);
    if (d) d->title_small = true;
}

/* 下载中的包名（net 模块的 s_dl_name 是"文件名主干"）与某个包名是否同一个 */
static bool dl_name_is(const char *pkg)
{
    if (!pkg || !pkg[0]) return false;
    const char *dn = am_rstore_dl_name();
    if (!dn || !dn[0]) return false;
    char stem[56];
    snprintf(stem, sizeof(stem), "%s", pkg);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    return strcmp(stem, dn) == 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * 列表投影：应用段（统一核心） + 资源段（本文件）
 * ════════════════════════════════════════════════════════════════════════ */

void am_store_prefetch(void)
{
    /* 只在"还没有数据"时踢扫描 —— 进商店/切分类会频繁调到这里，
     * 每次都重扫 10 个目录 + 逐个解析 LAPP 头会把切分类拖慢（旧实现有同款教训）。
     * 真正需要重扫的时机（安装/卸载/下载完成/插拔卡）各自显式调
     * am_store_invalidate()，语义清楚，不会互相吃掉。 */
    if (!am_core_ready()) am_core_invalidate();
    res_sync();
    extern void am_rstore_refresh_force(void);
    extern bool am_rstore_refresh_due(void);
    if (am_rstore_refresh_due()) am_rstore_refresh_force();
}

void am_store_refresh_index_if_due(void)
{
    extern bool am_rstore_refresh_due(void);
    if (am_rstore_refresh_due()) am_rstore_refresh_force();
}

bool am_store_ready(void) { return am_core_ready(); }

bool am_store_tick(void)
{
    bool changed = am_core_tick();
    /* res_sync 自带"代次没变 + 未失效就早退"，可以每帧调 ——
     * 安装/卸载后 am_store_invalidate() 置脏，这里会重新回验文件清单。 */
    if (s_res_dirty || am_rstore_gen() != s_res_gen_done) {
        /* ★ 2026-11-03: 索引换了(云端拉到 / 内置兜底加载) ⇒ 必须让 core 重扫一次。
         *   merge_index 才会把"未安装的目录条目"建出来；否则商店永远只显示**已安装**
         *   的应用（用户报：商店里只有已安装的，没安装的没拉下来）。 */
        am_core_invalidate();
        res_sync();
        changed = true;
    }
    return changed;
}

void am_store_invalidate(void)
{
    am_core_invalidate();
    s_res_dirty = true;
}

int am_store_count(void) { return am_core_count_store() + s_res_n; }

const os_app_t *am_store_app(int k)
{
    const int na = am_core_count_store();
    if (k >= 0 && k < na) {
        const am_app_t *e = am_core_store_at(k);
        return e ? &e->view : NULL;
    }
    const int r = k - na;
    if (r >= 0 && r < s_res_n) return &s_res[r].app;
    return NULL;
}

bool am_store_owns(const os_app_t *a)
{
    return am_core_from_view(a) != NULL || res_find(a) != NULL;
}

bool am_store_installed_of(const os_app_t *a)
{
    return am_store_state_of(a) != AM_ST_NONE;
}

const char *am_store_fname_of(const os_app_t *a)
{
    const am_app_t *e = am_core_from_view(a);
    if (e && e->pkg[0]) return e->pkg;
    am_res_row_t *r = res_find(a);
    return r ? res_stem(r) : NULL;
}

int am_store_app_where(const os_app_t *a)
{
    const am_app_t *e = am_core_from_view(a);
    if (!e) return 0;
    if (e->path_tf[0] && (e->loc & AM_LOC_TF))      return 2;
    if (e->path_flash[0] && (e->loc & AM_LOC_FLASH)) return 1;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * 状态 / 文案（应用段来自统一核心，资源段来自安装清单）
 * ════════════════════════════════════════════════════════════════════════ */

am_st_t am_store_state_of(const os_app_t *a)
{
    const am_app_t *e = am_core_from_view(a);
    if (e) {
        am_st_t base = am_core_state_of(e);
        if (am_rstore_state() == ARS_DOWNLOADING && dl_name_is(e->pkg))
            return am_st_with_task(base, AM_TASK_DL);
        if (s_uninst_busy && strcmp(s_uninst_key, e->key) == 0)
            return am_st_with_task(base, AM_TASK_UNINSTALL);
        return base;
    }
    am_res_row_t *r = res_find(a);
    if (!r) return AM_ST_NONE;
    if (s_uninst_busy && strncmp(s_uninst_key, r->id, sizeof(s_uninst_key) - 1) == 0)
        return AM_ST_UNINSTALLING;
    if (am_rstore_state() == ARS_DOWNLOADING && dl_name_is(res_stem(r)))
        return AM_ST_DL;
    return r->installed ? AM_ST_INSTALLED : AM_ST_NONE;
}

const char *am_store_btn_of(const os_app_t *a)
{
    return am_st_btn(am_store_state_of(a));
}

const char *am_store_sub_of(const os_app_t *a)
{
    const am_app_t *e = am_core_from_view(a);
    if (e) {
        if (e->desc[0]) return e->desc;
        if (e->size_remote) {
            static char b[40];
            snprintf(b, sizeof(b), "%u KB", (unsigned)((e->size_remote + 1023u) / 1024u));
            return b;
        }
        return NULL;
    }
    am_res_row_t *r = res_find(a);
    if (!r) return NULL;
    const am_rpkg_t *rp = am_rstore_get(r->ridx);
    static char b[40];
    if (rp && rp->desc[0]) return rp->desc;
    if (rp && rp->size) {
        snprintf(b, sizeof(b), "%u KB", (unsigned)((rp->size + 1023u) / 1024u));
        return b;
    }
    return NULL;
}

int am_store_dl_permille_of(const os_app_t *a)
{
    if (am_rstore_state() != ARS_DOWNLOADING) return -1;
    const char *fn = am_store_fname_of(a);
    if (!fn || !dl_name_is(fn)) return -1;
    return am_rstore_dl_permille();
}

/* ════════════════════════════════════════════════════════════════════════
 * 资源包依赖（requires=）：安装主程序后顺带把素材拉下来
 * ════════════════════════════════════════════════════════════════════════ */

static void req_split(const char *req, char *path, size_t cap, uint32_t *minver)
{
    if (minver) *minver = 0;
    if (path && cap) path[0] = 0;
    if (!req || !req[0]) return;
    const char *at = strchr(req, '@');
    size_t n = at ? (size_t)(at - req) : strlen(req);
    if (path && cap) {
        if (n >= cap) n = cap - 1;
        memcpy(path, req, n);
        path[n] = 0;
    }
    if (at && minver) *minver = (uint32_t)strtoul(at + 1, NULL, 10);
}

static int req_find(const char *path)
{
    if (!path || !path[0]) return -1;
    for (int i = 0; i < am_rstore_count(); i++) {
        const am_rpkg_t *rp = am_rstore_get(i);
        if (!rp || !am_pkg_is_res(rp->type)) continue;
        if (strcmp(rp->path, path) == 0) return i;
    }
    return -1;
}

static bool req_ready(const char *req)
{
    char path[96];
    uint32_t minver = 0;
    req_split(req, path, sizeof(path), &minver);
    int i = req_find(path);
    if (i < 0) return false;
    const am_rpkg_t *rp = am_rstore_get(i);
    if (!rp || !rp->target[0]) return false;
    if (minver && rp->ver < minver) return false;
    return am_asset_installed_at(rp->target, rp->path);
}

/* 1=已就绪 0=已开始下载 -1=索引里没有这条资源包 */
static int req_ensure(const char *req, bool start_dl)
{
    if (!req || !req[0]) return 1;
    if (req_ready(req)) return 1;
    char path[96];
    uint32_t minver = 0;
    req_split(req, path, sizeof(path), &minver);
    int i = req_find(path);
    if (i < 0) return -1;
    if (!start_dl) return 0;
    return am_rstore_dl_start(i) ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * 下载 / 更新
 * ════════════════════════════════════════════════════════════════════════ */

/* 索引下标可能因为刷新而变，按包名重定位（老实现踩过"用缓存下标就下错包"）。 */
static int ridx_of(const os_app_t *a)
{
    if (!a) return -1;
    const am_app_t *e = am_core_from_view(a);
    am_res_row_t *r = res_find(a);

    /* 主标识: 已装/目录项的包文件名主干；否则资源包的 zip 文件名主干 */
    static char s_stem[56];
    const char *stem = NULL;
    if (e && e->pkg[0]) {
        snprintf(s_stem, sizeof(s_stem), "%s", e->pkg);
        char *dot = strrchr(s_stem, '.'); if (dot) *dot = 0;
        stem = s_stem;
    } else if (r) {
        snprintf(s_stem, sizeof(s_stem), "%s", res_stem(r));
        char *dot = strrchr(s_stem, '.'); if (dot) *dot = 0;
        stem = s_stem;
    }
    if (!stem && !(a->label && a->label[0])) return -1;

    for (int i = 0; i < am_rstore_count(); i++) {
        const am_rpkg_t *rp = am_rstore_get(i);
        if (!rp) continue;
        /* 目录里的 name 匹配：文件名主干 或 界面 label（缓冲换代指针失效时用 label 兜底） */
        if (rp->name[0]) {
            if (stem && strcmp(rp->name, stem) == 0) return i;
            if (a->label && a->label[0] && strcmp(rp->name, a->label) == 0) return i;
        }
        /* ★ 资源包(字体/素材)的 name 是显示名(文泉驿点阵宋12)，与文件名(font_wqy12)不同
         *   ⇒ 再按 path 的文件名主干比一次；否则"下载字体"会报"商店目录里没有这个包"。 */
        if (stem) {
            const char *sl = strrchr(rp->path, '/');
            const char *base = sl ? sl + 1 : rp->path;
            char bs[56];
            snprintf(bs, sizeof(bs), "%s", base);
            char *bd = strrchr(bs, '.'); if (bd) *bd = 0;
            if (strcmp(bs, stem) == 0) return i;
        }
    }
    return -1;
}

static void dl_start_cb(ui_ctx_t *ctx, int result, void *ud)
{
    if (result != 0) { ctx->needs_redraw = true; return; }
    int rk = (int)(long)ud;
    if (rk < 0 || rk >= am_rstore_count() || !am_rstore_dl_start(rk))
        os_dialog_toast(ctx, "\xe6\x97\xa0\xe6\xb3\x95\xe5\xbc\x80\xe5\xa7\x8b\xe4\xb8\x8b\xe8\xbd\xbd"); /* 无法开始下载 */
    am_rebuild_keep();
    ctx->needs_redraw = true;
}

/* 未联网时不弹"下载 XX?"，直接提示 —— 少一次无意义点击（2026-10-18 定）。 */
static void download_ask(ui_ctx_t *ctx, const os_app_t *a)
{
    if (!wifi_manager_is_connected()) {
        os_dialog_toast(ctx, "\xe8\xaf\xb7\xe8\xbf\x9e\xe6\x8e\xa5\xe7\xbd\x91\xe7\xbb\x9c"); /* 请连接网络 */
        return;
    }
    int rk = ridx_of(a);
    if (rk < 0) { os_dialog_toast(ctx, "\xe5\x95\x86\xe5\xba\x97\xe7\x9b\xae\xe5\xbd\x95\xe9\x87\x8c\xe6\xb2\xa1\xe6\x9c\x89\xe8\xbf\x99\xe4\xb8\xaa\xe5\x8c\x85"); return; } /* 商店目录里没有这个包 */
    char title[72];
    snprintf(title, sizeof(title), "\xe4\xb8\x8b\xe8\xbd\xbd %s?", a->label ? a->label : "");   /* 下载 XX? */
    os_dialog_confirm_ex(ctx, title, 0, 0, dl_start_cb, (void *)(long)rk);
    dlg_title16(ctx);
}

/* 更新 = 同一个下载通道（下载完成后由下载任务落到正式安装位置）。
 * 先把商店源目录里可能残留的旧副本清掉，否则"装完还是老版本"。 */
static void update_ask(ui_ctx_t *ctx, const os_app_t *a)
{
    const am_app_t *e = am_core_from_view(a);
    if (!e) { download_ask(ctx, a); return; }
    if (!wifi_manager_is_connected()) {
        os_dialog_toast(ctx, "\xe8\xaf\xb7\xe8\xbf\x9e\xe6\x8e\xa5\xe7\xbd\x91\xe7\xbb\x9c");
        return;
    }
    char stale[192];
    snprintf(stale, sizeof(stale), "%s/%s", D_TF_STORE_PKG, e->pkg);
    remove(stale);
    int rk = ridx_of(a);
    if (rk < 0 || !am_rstore_dl_start(rk))
        os_dialog_toast(ctx, "\xe6\x97\xa0\xe6\xb3\x95\xe5\xbc\x80\xe5\xa7\x8b\xe4\xb8\x8b\xe8\xbd\xbd");
    am_rebuild_keep();
}

/* ════════════════════════════════════════════════════════════════════════
 * 安装（本地已有包，非下载路径）
 * ════════════════════════════════════════════════════════════════════════ */

static const char *s_install_key;   /* 对话框是异步的，指针存 static 且只用于本次回调 */

static void install_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    const char *key = s_install_key;
    s_install_key = NULL;
    if (result != 0 || !key) { ctx->needs_redraw = true; return; }
    char why[96] = {0};
    if (am_core_install(key, why, sizeof(why)) == ESP_OK) {
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85");   /* 已安装 */
        /* 主程序声明的资源依赖：顺手拉下来，否则"装好了但打开缺素材" */
        const am_app_t *e = am_core_find(key);
        if (e && e->ridx >= 0) {
            const am_rpkg_t *rp = am_rstore_get(e->ridx);
            if (rp && rp->requires[0]) req_ensure(rp->requires, true);
        }
    } else {
        char m[96];
        snprintf(m, sizeof(m), "\xe5\xae\x89\xe8\xa3\x85\xe5\xa4\xb1\xe8\xb4\xa5: %s", why);  /* 安装失败: */
        os_dialog_toast(ctx, m);
    }
    am_rebuild_keep();
    ctx->needs_redraw = true;
}

static void install_ask(ui_ctx_t *ctx, const os_app_t *a)
{
    const am_app_t *e = am_core_from_view(a);
    if (!e) return;
    char title[72];
    snprintf(title, sizeof(title), "\xe5\xae\x89\xe8\xa3\x85 %s ?", a->label ? a->label : "");  /* 安装 XXX ? */
    s_install_key = e->key;
    os_dialog_confirm_ex(ctx, title, 0, 0, install_cb, NULL);
    dlg_title16(ctx);
}

/* ════════════════════════════════════════════════════════════════════════
 * 卸载
 * ════════════════════════════════════════════════════════════════════════ */

/* 卸载后要让「人生指南壁纸」立刻失效：数据预加载在 PSRAM 里，只删文件不释放
 * ⇒ 屏保继续显示旧内容，直到重启。只在这一个卸载入口判一次，不建通用通知框架。 */
static void post_uninstall_cleanup(const char *rt_id)
{
    if (rt_id && rt_id[0]) {
        os_app_set_mainmenu(rt_id, false);
        os_app_set_hidden(rt_id, true);
    }
    if (!wp_lifeguide_installed()) {
        wp_lifeguide_release();
        if (svc_cfg_get_i32("os_wp", "prog", WP_PROG_STARS) == WP_PROG_LIFEGUIDE) {
            svc_cfg_set_i32("os_wp", "prog", WP_PROG_STARS);
            wp_set_program(WP_PROG_STARS);
        }
    }
}

typedef struct { char key[AM_CORE_KEY_LEN]; char rid[AM_CORE_KEY_LEN]; } uninst_arg_t;

static void uninst_do(uninst_arg_t *p)
{
    char why[96] = {0};
    esp_err_t r = am_core_remove(p->key, true, why, sizeof(why));
    ESP_LOGI(TAG, "卸载 %s: %s %s", p->key, esp_err_to_name(r), why);
    post_uninstall_cleanup(p->rid);
    s_uninst_busy = false;
    s_uninst_key[0] = 0;
    s_uninst_rid[0] = 0;
    s_uninst_done = true;
    am_store_invalidate();
}

static void uninst_task(void *arg)
{
    uninst_arg_t *p = (uninst_arg_t *)arg;
    uninst_do(p);
    free(p);
    vTaskDelete(NULL);
}

typedef struct { char zip[96]; char dir[96]; } asset_rm_arg_t;

static void asset_uninst_do(asset_rm_arg_t *p)
{
    if (am_is_lpk_path(p->zip)) {
        char id[64];
        am_lpkg_stem_of(p->zip, id, sizeof(id));
        if (id[0] && am_lpkg_is_installed(id)) {
            char why[96] = {0};
            esp_err_t e = am_lpkg_uninstall(id, true, why, sizeof(why));
            ESP_LOGW(TAG, "资源包(LPK) 卸载 %s: %s", id, esp_err_to_name(e));
            goto done;
        }
        if (!p->dir[0]) {
            ESP_LOGE(TAG, "拒绝卸载 %s: registry 无记录且无 target 目录", p->zip);
            goto done;
        }
    }
    if (!am_asset_uninstall(p->zip, p->dir)) am_asset_rmdir(p->dir);
done:
    s_uninst_busy = false;
    s_uninst_key[0] = 0;
    s_uninst_done = true;
    am_store_invalidate();
}

static void asset_uninst_task(void *arg)
{
    asset_rm_arg_t *p = (asset_rm_arg_t *)arg;
    asset_uninst_do(p);
    free(p);
    vTaskDelete(NULL);
}

static void start_uninst(const char *key, const char *rid)
{
    uninst_arg_t *p = calloc(1, sizeof(*p));
    if (!p) return;
    snprintf(p->key, sizeof(p->key), "%s", key ? key : "");
    snprintf(p->rid, sizeof(p->rid), "%s", rid ? rid : "");
    snprintf(s_uninst_key, sizeof(s_uninst_key), "%s", p->key);
    snprintf(s_uninst_rid, sizeof(s_uninst_rid), "%s", p->rid);
    s_uninst_busy = true;
    if (xTaskCreate(uninst_task, "am_uninst", 6144, p, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "卸载任务创建失败, 改同步删");
        uninst_do(p);          /* 只调工作体 —— 在 UI 任务里 vTaskDelete 会把主循环删掉 */
        free(p);
    }
}

static void start_asset_uninst(const char *zip, const char *dir, const char *keyid)
{
    asset_rm_arg_t *p = calloc(1, sizeof(*p));
    if (!p) return;
    snprintf(p->zip, sizeof(p->zip), "%s", zip ? zip : "");
    snprintf(p->dir, sizeof(p->dir), "%s", dir ? dir : "");
    snprintf(s_uninst_key, sizeof(s_uninst_key), "%s", keyid ? keyid : "");
    s_uninst_busy = true;
    if (xTaskCreate(asset_uninst_task, "am_asset_rm", 4096, p, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "资源包卸载任务创建失败, 改同步删");
        asset_uninst_do(p);
        free(p);
    }
}

static void uninst_cb(ui_ctx_t *ctx, int result, void *ud)
{
    /* ⚠ 回调里绝不能 os_dialog_pop：框架会在回调返回后自己关框，
     *   手动 pop 在 s_top==0 时走的是 os_pop() —— 会把商店页一起退掉。 */
    if (result != 0) { ctx->needs_redraw = true; free(ud); return; }
    uninst_arg_t *p = (uninst_arg_t *)ud;
    if (!p) return;
    start_uninst(p->key, p->rid);
    free(p);
    ctx->needs_redraw = true;
}

static void asset_uninst_cb(ui_ctx_t *ctx, int result, void *ud)
{
    asset_rm_arg_t *p = (asset_rm_arg_t *)ud;
    if (result != 0 || !p) { ctx->needs_redraw = true; free(p); return; }
    char id[64];
    {
        char stem[56];
        am_lpkg_stem_of(p->zip, stem, sizeof(stem));
        snprintf(id, sizeof(id), "res:%s", stem);   /* 与 s_res[].id 同构，按钮才显示"卸载中" */
    }
    char zip[96], dir[96];
    snprintf(zip, sizeof(zip), "%s", p->zip);
    snprintf(dir, sizeof(dir), "%s", p->dir);
    free(p);
    start_asset_uninst(zip, dir, id);
    ctx->needs_redraw = true;
}

void am_store_uninstall_ask(ui_ctx_t *ctx, const os_app_t *a)
{
    if (!a) return;
    const am_app_t *e = am_core_from_view(a);
    if (e) {
        am_st_t st = am_store_state_of(a);
        if (st != AM_ST_INSTALLED && st != AM_ST_UPDATE && st != AM_ST_ORPHAN) return;
        uninst_arg_t *p = calloc(1, sizeof(*p));
        if (!p) return;
        snprintf(p->key, sizeof(p->key), "%s", e->key);
        snprintf(p->rid, sizeof(p->rid), "%s", e->rt_id);
        char msg[72];
        snprintf(msg, sizeof(msg), "\xe5\x8d\xb8\xe8\xbd\xbd %s?", a->label ? a->label : "");  /* 卸载 XX? */
        os_dialog_confirm_ex(ctx, msg, 0, 0, uninst_cb, p);
        dlg_title16(ctx);
        return;
    }

    am_res_row_t *r = res_find(a);
    if (!r) return;
    if (am_store_state_of(a) != AM_ST_INSTALLED) return;

    /* 正在被阅读器使用的字体不许卸 —— 删掉当前字库会立刻变成一排缺字框。
     * 只按**本包的安装清单**核对，不用目录前缀（同目录下常装多个字体）。 */
    if (r->target[0]) {
        const char *using_f = font_book_current_file();
        if (using_f && using_f[0]) {
            bool mine = am_asset_has_file(r->zip, r->target, using_f);
            if (!mine) {
                size_t dl = strlen(r->target);
                bool under = (strncmp(using_f, r->target, dl) == 0 &&
                              (using_f[dl] == '/' || using_f[dl] == 0));
                mine = under && !r->zip[0];
            }
            if (mine) {
                os_dialog_toast(ctx, "\xe8\xaf\xa5\xe5\xad\x97\xe4\xbd\x93\xe6\xad\xa3\xe5\x9c\xa8\xe4\xbd\xbf\xe7\x94\xa8\xef\xbc\x8c\xe8\xaf\xb7\xe5\x85\x88\xe5\x88\x87\xe6\x8d\xa2\xe5\xad\x97\xe4\xbd\x93");
                return;
            }
        }
    }
    asset_rm_arg_t *p = calloc(1, sizeof(*p));
    if (!p) return;
    snprintf(p->zip, sizeof(p->zip), "%s", r->zip);
    snprintf(p->dir, sizeof(p->dir), "%s", r->target);
    char msg[72];
    snprintf(msg, sizeof(msg), "\xe5\x8d\xb8\xe8\xbd\xbd %s?", a->label ? a->label : "");
    os_dialog_confirm_ex(ctx, msg, 0, 0, asset_uninst_cb, p);
    dlg_title16(ctx);
}

/* 长按商店行 → 卸载的唯一入口（与应用/资源分流，避免把资源包当 .lua 删） */
void am_store_uninstall_row(ui_ctx_t *ctx, const os_app_t *a)
{
    am_store_uninstall_ask(ctx, a);
}

bool am_store_take_uninst_done(void)
{
    if (!s_uninst_done) return false;
    s_uninst_done = false;
    return true;
}

/* ════════════════════════════════════════════════════════════════════════
 * 按钮动作
 * ════════════════════════════════════════════════════════════════════════ */

void am_store_toggle(ui_ctx_t *ctx, const os_app_t *a)
{
    if (!a) return;
    am_st_t st = am_store_state_of(a);

    if (am_st_btn_busy(st)) {
        os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe5\xa4\x84\xe7\x90\x86\xef\xbc\x8c\xe8\xaf\xb7\xe7\xa8\x8d\xe5\x80\x99"); /* 正在处理，请稍候 */
        return;
    }

    const am_app_t *e = am_core_from_view(a);
    am_res_row_t *r = res_find(a);

    switch (st) {
    case AM_ST_UPDATE:   update_ask(ctx, a); return;
    case AM_ST_INSTALLED:
    case AM_ST_ORPHAN:   am_store_uninstall_ask(ctx, a); return;
    case AM_ST_NONE:
    default: break;
    }

    /* 未安装 / 未下载 */
    if (r) { download_ask(ctx, a); return; }          /* 资源包只能从服务器装 */
    /* ★ 2026-11-04: 原来 `if (!e) return;` 会静默吞掉点击（缓冲换代后 am_core_from_view
     *   可能取不到指针）→ 用户"点下载没反应"。改为仍走下载兜底（ridx_of 用 label 兜底）。 */
    if (!e) { download_ask(ctx, a); return; }
    if (e->entry[0] && (e->kind == AM_KIND_LPK)) {
        /* 本地已有 .lpk 安装包（用户拷进来的）→ 直接装，不必再下 */
        install_ask(ctx, a);
        return;
    }
    if (e->in_catalog) { download_ask(ctx, a); return; }
    os_dialog_toast(ctx, "\xe6\xb2\xa1\xe6\x9c\x89\xe5\x8f\xaf\xe7\x94\xa8\xe7\x9a\x84\xe5\xae\x89\xe8\xa3\x85\xe5\x8c\x85"); /* 没有可用的安装包 */
}

/* ════════════════════════════════════════════════════════════════════════
 * 详情弹窗
 * ════════════════════════════════════════════════════════════════════════ */

static void detail_cb(ui_ctx_t *ctx, int r, void *ud) { (void)ctx; (void)r; (void)ud; }

void am_store_detail(ui_ctx_t *ctx, const os_app_t *a)
{
    if (!a) return;
    char buf[7][96];
    const char *ptrs[7];
    int n = 0;

    if (a->cat && a->cat[0]) {
        snprintf(buf[n], sizeof(buf[n]), "\xe5\x88\x86\xe7\xb1\xbb: %s", a->cat);   /* 分类: */
        ptrs[n] = buf[n]; n++;
    }

    const am_app_t *e = am_core_from_view(a);
    am_res_row_t *r = res_find(a);
    const am_rpkg_t *rp = NULL;
    if (e)       rp = (e->ridx >= 0) ? am_rstore_get(e->ridx) : NULL;
    else if (r)  rp = am_rstore_get(r->ridx);

    if (rp) {
        if (rp->ver) {
            snprintf(buf[n], sizeof(buf[n]), "\xe7\x89\x88\xe6\x9c\xac: %u", (unsigned)rp->ver);  /* 版本: */
            ptrs[n] = buf[n]; n++;
        }
        if (rp->author[0] && n < 7) {
            snprintf(buf[n], sizeof(buf[n]), "\xe4\xbd\x9c\xe8\x80\x85: %s", rp->author);        /* 作者: */
            ptrs[n] = buf[n]; n++;
        }
        if (rp->size && n < 7) {
            snprintf(buf[n], sizeof(buf[n]), "\xe5\xa4\xa7\xe5\xb0\x8f: %u KB",                  /* 大小: */
                     (unsigned)((rp->size + 1023u) / 1024u));
            ptrs[n] = buf[n]; n++;
        }
        if (rp->date[0] && n < 7) {
            snprintf(buf[n], sizeof(buf[n]), "\xe6\x97\xa5\xe6\x9c\x9f: %s", rp->date);          /* 日期: */
            ptrs[n] = buf[n]; n++;
        }
    }
    if (n < 7) {
        am_st_t st = am_store_state_of(a);
        const char *txt =
            (st == AM_ST_INSTALLED)    ? "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85"                            /* 已安装 */
          : (st == AM_ST_UPDATE)       ? "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85\xc2\xb7\xe5\x8f\xaf\xe6\x9b\xb4\xe6\x96\xb0" /* 已安装·可更新 */
          : (st == AM_ST_ORPHAN)       ? "\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8\xe5\xb7\xb2\xe4\xb8\x8b\xe6\x9e\xb6"      /* 服务器已下架 */
          : (st == AM_ST_DL)           ? "\xe4\xb8\x8b\xe8\xbd\xbd\xe4\xb8\xad"                            /* 下载中 */
          : (st == AM_ST_INSTALLING)   ? "\xe5\xae\x89\xe8\xa3\x85\xe4\xb8\xad"                            /* 安装中 */
          : (st == AM_ST_UPDATING)     ? "\xe6\x9b\xb4\xe6\x96\xb0\xe4\xb8\xad"                            /* 更新中 */
          : (st == AM_ST_UNINSTALLING) ? "\xe5\x8d\xb8\xe8\xbd\xbd\xe4\xb8\xad"                            /* 卸载中 */
                                       : "\xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85";                            /* 未安装 */
        snprintf(buf[n], sizeof(buf[n]), "\xe7\x8a\xb6\xe6\x80\x81: %s", txt);   /* 状态: */
        ptrs[n] = buf[n]; n++;
    }

    const char *ds = am_store_sub_of(a);
    if (ds && ds[0] && n < 7) {
        snprintf(buf[n], sizeof(buf[n]), "%s", ds);
        ptrs[n] = buf[n]; n++;
    }
    if (n == 0) return;
    os_dialog_list(ctx, a->label, ptrs, n, 0, detail_cb, NULL);
    dlg_title16(ctx);
}

/* ════════════════════════════════════════════════════════════════════════
 * 下载完成 → 安装（由 am_store_net.c 的下载任务调用）
 * ════════════════════════════════════════════════════════════════════════ */

bool am_store_auto_install(const char *fname)
{
    char why[96] = {0};
    esp_err_t r = am_core_install_downloaded(fname, why, sizeof(why));
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "自动安装失败 %s: %s %s", fname, esp_err_to_name(r), why);
        return false;
    }
    /* 主程序声明的资源依赖：顺手拉下来 */
    const am_app_t *e = NULL;
    for (int i = 0; i < am_core_count(); i++) {
        const am_app_t *c = am_core_at(i);
        if (c && strcmp(am_core_pkg_name_of(c), fname) == 0) { e = c; break; }
    }
    if (e && e->ridx >= 0) {
        const am_rpkg_t *rp = am_rstore_get(e->ridx);
        if (rp && rp->requires[0]) req_ensure(rp->requires, true);
    }
    am_store_invalidate();
    return true;
}

/* ════════════════════════════════════════════════════════════════════════
 * TF 卡 ↔ 内置 搬家
 * ════════════════════════════════════════════════════════════════════════ */

uint32_t am_store_internal_free(void)
{
    size_t total = 0, used = 0;
    if (esp_spiffs_info("apps", &total, &used) != ESP_OK) return 0;
    return (used >= total) ? 0u : (uint32_t)(total - used);
}

bool am_store_move_to_internal(const os_app_t *a, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    const am_app_t *e = am_core_from_view(a);
    if (!e) { if (err) snprintf(err, errlen, "找不到应用记录"); return false; }
    if (am_store_internal_free() < 16u * 1024u) {
        if (err) snprintf(err, errlen, "内置空间不足");
        return false;
    }
    char why[96] = {0};
    if (am_core_move_to(e->key, true, why, sizeof(why)) != ESP_OK) {
        if (err) snprintf(err, errlen, "%s", why[0] ? why : "移动失败");
        return false;
    }
    am_store_invalidate();
    return true;
}

bool am_store_move_to_sd(const os_app_t *a, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    const am_app_t *e = am_core_from_view(a);
    if (!e) { if (err) snprintf(err, errlen, "找不到应用记录"); return false; }
    char why[96] = {0};
    if (am_core_move_to(e->key, false, why, sizeof(why)) != ESP_OK) {
        if (err) snprintf(err, errlen, "%s", why[0] ? why : "移动失败");
        return false;
    }
    am_store_invalidate();
    return true;
}

/* 可否"搬到内置"：只有 Lua 单文件能搬（LPK 是解包目录 + registry，搬了要重装）。
 * 长按分流用它，避免对 LPK 弹出一个点下去必然失败的确认框。 */
bool am_store_movable(const os_app_t *a)
{
    const am_app_t *e = am_core_from_view(a);
    return e && e->kind == AM_KIND_LUA && (e->loc & AM_LOC_TF);
}

static const char *s_move_key;

static void move_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    const char *key = s_move_key;
    s_move_key = NULL;
    if (result != 0 || !key) return;
    char why[96] = {0};
    if (am_core_move_to(key, true, why, sizeof(why)) == ESP_OK) {
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe7\xa7\xbb\xe5\x8a\xa8\xe5\x88\xb0\xe5\x86\x85\xe7\xbd\xae\xe7\xa9\xba\xe9\x97\xb4"); /* 已移动到内置空间 */
        am_rebuild_keep();
    } else {
        os_dialog_toast(ctx, why[0] ? why : "\xe7\xa7\xbb\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5");   /* 移动失败 */
    }
}

void am_move_internal_ask(ui_ctx_t *ctx, const os_app_t *a)
{
    if (!a) return;
    const am_app_t *e = am_core_from_view(a);
    if (!e) return;
    uint32_t freeb = am_store_internal_free();
    if (freeb == 0) { os_dialog_toast(ctx, "\xe5\x86\x85\xe7\xbd\xae\xe7\xa9\xba\xe9\x97\xb4\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8"); return; }
    if (freeb < 16u * 1024u) {
        char m[64];
        snprintf(m, sizeof(m), "\xe5\x86\x85\xe7\xbd\xae\xe7\xa9\xba\xe9\x97\xb4\xe4\xb8\x8d\xe8\xb6\xb3(\xe4\xbd\x99%uKB)",
                 (unsigned)(freeb / 1024));
        os_dialog_toast(ctx, m);
        return;
    }
    s_move_key = e->key;
    os_dialog_confirm_ex(ctx, "\xe7\xa7\xbb\xe5\x8a\xa8\xe5\x88\xb0\xe5\x86\x85\xe7\xbd\xae\xe7\xa9\xba\xe9\x97\xb4?", 0, 0, move_cb, NULL);
}
