/**
 * am_app_core.c — 统一应用管理核心（实现，见 am_app_core.h 的五条定稿规则）
 *
 * 设计要点（改动前必读）
 * ----------------------
 * · **一次扫描、一份记录**：内置表 / Lua 目录 / 已装 LPK / 未装 LPK 包 / 服务器索引
 *   全部汇进 `s_app[]`，按 key 去重。页面不再自己扫、自己判状态。
 * · **后台扫描 + 影子缓冲**：沿用本项目已验证的模式（am_pkg_scan / am_lua_scan 同款），
 *   避免 SPIFFS opendir 阻塞首帧；换缓冲只换指针。
 * · **身份带类型前缀**：`lua:` / `lpk:` / `builtin:` ⇒ 同名 Lua 与 LPK 永不合并；
 *   同类型同规范化值 ⇒ Flash/TF 副本自然合并成一条。
 * · **TF 可访问性独立**：`storage_ok` 只影响"能不能显示/启动"，不改安装事实；
 *   拔卡后 TF-only 条目从应用中心过滤掉，但记录与 registry 事实保留。
 * · **不 cast**：页面要的 `os_app_t` 是 `am_app_t.view`，由 build_views() 填，
 *   指向本结构自己的字段（与 am_pkg_entry_t.app 同一套做法）。
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "am_app_core.h"
#include "am_lpkg.h"
#include "am_internal.h"
#include "am_store_json.h"      /* am_rpkg_t + am_rstore_count/get */
#include "apppack.h"
#include "lpk_manifest.h"
#include "apps.h"
#include "ui_common.h"          /* ui_main_icon_bitmap / ui_icon_register_extra */

static const char *TAG = "APPCORE";

/* ───────── 目录（与既有实现完全一致，不新增"猜"的目录） ───────── */
#define D_FLASH_APPS   "/store/apps"
#define D_TF_APPS      "/sdcard/apps"
#define D_TF_INSTALLED "/sdcard/apps/installed"
#define D_TF_STORE_PKG "/sdcard/store_pkg"
#define D_FLASH_ROOT   "/store"

/* 双缓冲 + **指针交换**（不是 memcpy）。
 * 为什么用交换：页面的 s_list[] 里存着 &s_app[i].view 指针。memcpy 会把同一块内存
 * 的内容换掉，那一帧里"第 3 行"可能已经变成另一个应用（点了会跑错 App）。
 * 交换指针则让旧缓冲整份保留到下一帧，指针内容前后一致。 */
static EXT_RAM_BSS_ATTR am_app_t s_buf_a[AM_CORE_MAX];
static EXT_RAM_BSS_ATTR am_app_t s_buf_b[AM_CORE_MAX];
static am_app_t *s_app = s_buf_a;   /* 读侧（页面用） */
static am_app_t *s_bg  = s_buf_b;   /* 写侧（扫描任务用） */
static int  s_n = 0;            /* 读侧条数 */
static int  s_bg_n = 0;         /* 写侧条数 */
static volatile bool s_bg_ready = false;
static volatile bool s_busy = false;
static volatile bool s_ready = false;
static volatile bool s_dirty = true;
static volatile bool s_touch = false;   /* 定向修改过读侧数据 ⇒ 下次 tick 要报"有变化" */
/* ★ 数据代次：每次读侧内容（s_app[]）真正换代就 +1。
 *   页面用它判断"我手上这份列表还作数吗" —— 因为页面列表存的是
 *   &s_app[i].view **指针**，双缓冲一交换，旧指针就指向被弃用的缓冲了。
 *   （真实故障：卸载后说明整片消失，切分类才恢复。）
 *   页面构建列表时记下当时的代次；代次变了 ⇒ 必须重建（不能靠"分类没变"复用）。 */
static volatile uint32_t s_gen = 0;
static bool s_tf_seen = false;      /* 上一次扫描看到的 TF 可用性（插拔检测用） */
static int64_t s_scan_retry_at = 0; /* >0 = 这次扫描失败，到这个时刻再重试（微秒） */
/* 后台卸载完成后要把"这一条"改成未安装 —— 但读侧缓冲只能在 UI 线程改，
 * 所以后台只投递 key，由 am_core_tick 代劳。 */
/* 后台卸载投递的 key（由 am_core_remove 写、由 am_core_tick 消费） */
static char s_pending_rm[AM_CORE_KEY_LEN];
static volatile bool s_pending_rm_set = false;

/* 前向声明：apply_removed 定义在本文件下方，am_core_tick 要用它。 */
static void apply_removed(const char *key);

/* ───────── 小工具 ───────── */

static bool exists(const char *p) { struct stat st; return p && p[0] && stat(p, &st) == 0; }
static bool tf_avail(void) { return exists("/sdcard"); }

/* 身份归一 / 状态判定 / 投影判据全部在 am_core_logic.c（纯 C，宿主机可测）：
 *   am_core_norm / am_core_make_key / am_core_same / am_core_decide /
 *   am_core_center_ok / am_core_store_ok
 * 本文件只负责"把真实文件系统与注册表喂给它们"。 */

/* ───────── 记录装配 ───────── */

static am_app_t *find_key_bg(const char *key)
{
    for (int i = 0; i < s_bg_n; i++)
        if (strcmp(s_bg[i].key, key) == 0) return &s_bg[i];
    return NULL;
}

static am_app_t *add_bg(am_kind_t kind, const char *raw, const char *key_in)
{
    char key[AM_CORE_KEY_LEN];
    if (key_in && key_in[0]) snprintf(key, sizeof(key), "%s", key_in);
    else am_core_make_key(kind, raw, key, sizeof(key));

    am_app_t *e = find_key_bg(key);
    if (e) return e;                       /* 同身份 ⇒ 合并（Flash/TF 走这里） */
    if (s_bg_n >= AM_CORE_MAX) return NULL;
    e = &s_bg[s_bg_n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->kind = kind;
    e->valid = true;
    e->ridx = -1;
    return e;
}

/* 名字优先级：中文 > 英文 > 文件名（规则 ⑥） */
static void set_names(am_app_t *e, const char *zh, const char *en, const char *fname)
{
    if (zh && zh[0])  snprintf(e->name_zh, sizeof(e->name_zh), "%s", zh);
    if (en && en[0])  snprintf(e->name_en, sizeof(e->name_en), "%s", en);
    const char *pick = (e->name_zh[0]) ? e->name_zh
                     : (e->name_en[0]) ? e->name_en : (fname ? fname : "");
    snprintf(e->name_show, sizeof(e->name_show), "%s", pick);
}

static const char *cat_zh_of(const char *cat)
{
    if (!cat || !cat[0]) return "软件";
    if (strcmp(cat, "工具") == 0) return "运维";
    return cat;
}

/* 分类 → 默认图标（与商店/桌面同一套口径，保证同一个应用三处长得一样） */
static int cat_icon_of(const char *cat)
{
    if (!cat || !cat[0])            return ICON_APP_MGR;
    if (strcmp(cat, "游戏") == 0)   return ICON_MINI_GAMES;
    if (strcmp(cat, "运维") == 0)   return ICON_TERMINAL;
    if (strcmp(cat, "标准") == 0)   return ICON_SETTINGS;
    if (strcmp(cat, "学习") == 0)   return ICON_BOOK;
    if (strstr(cat, "壁纸") || strcmp(cat, "资源") == 0 ||
        strcmp(cat, "wallpaper") == 0) return ICON_WALLPAPER;
    return ICON_APP_MGR;
}

/* 图标优先级：①LAPP 自带位图（反查主图标表，查不到就注册进运行时外来图标表）
 *              ②文件名专属图标（纯 .lua 没打包的老应用）③分类默认 */
static int icon_of(const apppack_meta_t *m, const char *fname, const char *cat)
{
    if (m && m->has_icon) {
        for (int i = 0; i < MAIN_ICON_COUNT; i++) {
            int w = 0, h = 0;
            const uint8_t *b = ui_main_icon_bitmap(i, &w, &h);
            if (b && w == MAIN_ICON_W && h == MAIN_ICON_H &&
                memcmp(b, m->icon, APPPACK_ICON_BYTES) == 0) return i;
        }
        int ex = ui_icon_register_extra(m->icon);
        if (ex >= 0) return ex;
    }
    if (fname) {
        static const struct { const char *f; int ic; } T[] = {
            { "BIOS模拟器.lua", ICON_BIOSSIM    },
            { "航班雷达.lua",   ICON_PLANERADAR },
        };
        for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
            if (strcasecmp(T[i].f, fname) == 0) return T[i].ic;
    }
    return cat_icon_of(cat);
}

/* ───────── 扫描：内置 C ───────── */

static void scan_builtin(void)
{
    const int n = apps_builtin_count();
    for (int i = 0; i < n; i++) {
        const os_app_t *b = apps_builtin_at(i);
        if (!b || !b->id) continue;
        am_app_t *e = add_bg(AM_KIND_BUILTIN, b->id, NULL);
        if (!e) continue;
        e->valid = true;
        snprintf(e->rt_id, sizeof(e->rt_id), "%s", b->id);   /* 与 apps_registry 注册的 id 一致 */
        e->loc = AM_LOC_FLASH;             /* 内置 ⇒ 恒可用 */
        e->storage_ok = true;
        set_names(e, b->label, NULL, b->id);
        snprintf(e->cat_zh, sizeof(e->cat_zh), "%s", cat_zh_of(b->cat));
        e->icon_idx = b->icon_idx;
        e->page     = b->page;
        snprintf(e->entry, sizeof(e->entry), "page:%d", (int)b->page);
    }
}

/* ───────── 扫描：Lua（Flash + TF，按 key 合并） ───────── */

static void scan_lua_dir(const char *base, const char *folder, bool is_tf)
{
    char dir[128];
    snprintf(dir, sizeof(dir), "%s/%s", base, folder);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        size_t len = strlen(nm);
        if (len <= 5 || strcasecmp(nm + len - 4, ".lua") != 0) continue;

        char path[AM_CORE_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, nm);

        /* 有效性：必须带 LAPP 头（`--@app`）—— 普通 .lua 脚本不算正式应用。
         * ⚠ **不要求 name=**：老包/工具生成的包可能只写了 cat=，显示名退回文件名即可。
         *   原来这里要求 name 非空，结果 `录音机.lua`（只有 cat= 的包）被整条丢掉，
         *   用户看到的是"文件明明在卡里，应用中心却没有"。 */
        apppack_meta_t m;
        if (apppack_parse(path, &m) != ESP_OK) continue;

        am_app_t *e = add_bg(AM_KIND_LUA, nm, NULL);
        if (!e) break;
        /* 运行期 id 必须与历史 NVS 里写的一致（`lua:<完整文件名>`），
         * 否则用户以前"添加到桌面"的图标会全部失效。 */
        snprintf(e->rt_id, sizeof(e->rt_id), "lua:%s", nm);
        /* ★ 2026-11-04 统一规则：Lua 应用「显示名 = 文件名（去扩展名）」。
         *   用户定：应用名称统一直接显示文件名称 —— 商店识别、应用中心显示名、
         *   桌面图标 id 全部以文件名为唯一真相源，消除「文件名≠名称」错配
         *   （典型就是预置 planeradar.lua 与商店 航班雷达.lua 对不上）。
         *   因此忽略包内 --@app name=，只用文件名。 */
        char nm_base[AM_CORE_PATH_LEN];
        size_t nl = strlen(nm);
        if (nl > 4 && strcasecmp(nm + nl - 4, ".lua") == 0) nl -= 4;
        snprintf(nm_base, sizeof(nm_base), "%.*s", (int)nl, nm);
        set_names(e, nm_base, NULL, NULL);
        snprintf(e->pkg, sizeof(e->pkg), "%s", nm);
        snprintf(e->desc, sizeof(e->desc), "%s", m.desc);
        snprintf(e->cat_zh, sizeof(e->cat_zh), "%s", cat_zh_of(m.cat));
        snprintf(e->ver_local, sizeof(e->ver_local), "%s", m.version);
        snprintf(e->build_local, sizeof(e->build_local), "%s", m.build);
        e->icon_idx = icon_of(&m, nm, m.cat);
        e->need_wifi = m.need_wifi;
        e->storage_ok = true;

        if (is_tf) {
            snprintf(e->path_tf, sizeof(e->path_tf), "%s", path);
            e->loc |= AM_LOC_TF;
            if (!e->entry[0]) snprintf(e->entry, sizeof(e->entry), "%s", path);
        } else {
            snprintf(e->path_flash, sizeof(e->path_flash), "%s", path);
            e->loc |= AM_LOC_FLASH;
            snprintf(e->entry, sizeof(e->entry), "%s", path);   /* Flash 优先作为入口 */
        }
    }
    closedir(d);
}

static void scan_lua(bool tf)
{
    static const char *const folders[] = { "engine", "app", "ops" };
    for (int f = 0; f < 3; f++) {
        scan_lua_dir(D_FLASH_APPS, folders[f], false);
        if (tf) scan_lua_dir(D_TF_APPS, folders[f], true);
    }
    /* apps 根目录下的手动副本（旧 am_lua_scan 不扫这里，导致"商店有、应用中心没有"） */
    scan_lua_dir(D_FLASH_ROOT, "", false);
    if (tf) scan_lua_dir(D_TF_APPS, "", true);
}

/* ───────── 扫描：已安装 LPK（registry 为准） ───────── */

static void scan_lpk_installed(void)
{
    const int n = am_lpkg_count();
    for (int i = 0; i < n; i++) {
        const char *id = am_lpkg_id_at(i);
        if (!id || !id[0]) continue;
        am_lpkg_info_t inf;
        if (!am_lpkg_info(id, &inf)) continue;

        am_app_t *e = add_bg(AM_KIND_LPK, id, NULL);
        if (!e) break;
        snprintf(e->rt_id, sizeof(e->rt_id), "%s", id);      /* app_id 原样，与 NVS/registry 一致 */
        /* 名称/分类取**包内 manifest**（registry 只存版本与来源，没有名称） */
        char mfp[AM_CORE_PATH_LEN];
        snprintf(mfp, sizeof(mfp), "%s/%s/manifest.json", D_TF_INSTALLED, id);
        lpk_mf_t mf;
        char why[64] = {0};
        if (lpk_mf_load(mfp, &mf, why, sizeof(why)) == LPK_MF_OK) {
            set_names(e, mf.label, mf.name, id);
            snprintf(e->cat_zh, sizeof(e->cat_zh), "%s", cat_zh_of(mf.category));
            /* ★ 说明必须来自包内 manifest；没有就留空，⛔ 绝不拿名称/路径/分类冒充说明 */
            if (!e->desc[0]) snprintf(e->desc, sizeof(e->desc), "%s", mf.description);
            e->need_wifi = mf.has_permissions;
        } else {
            set_names(e, id, NULL, id);
        }
        snprintf(e->ver_local, sizeof(e->ver_local), "%s", inf.ver);
        snprintf(e->build_local, sizeof(e->build_local), "%s", inf.build);
        snprintf(e->pkg, sizeof(e->pkg), "%s.lpk", id);
        e->loc |= AM_LOC_TF;                    /* LPK 安装根恒为 /sdcard */
        e->storage_ok = true;
        if (!am_lpkg_entry_path(id, e->entry, sizeof(e->entry)))
            e->valid = false;                    /* 入口找不到 ⇒ 无效，不进列表 */
    }
}

/* ───────── 扫描：未安装的 .lpk 安装包（只进商店） ───────── */

static void scan_lpk_pkg_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        size_t len = strlen(nm);
        if (len <= 5 || strcasecmp(nm + len - 4, ".lpk") != 0) continue;

        char path[AM_CORE_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, nm);

        lpk_mf_t mf;
        char why[64] = {0};
        if (lpk_mf_load(path, &mf, why, sizeof(why)) != LPK_MF_OK) continue;
        if (strcmp(mf.package_type, "app") != 0) continue;      /* 资源包不进应用列表 */
        if (mf.runtime[0] && strcmp(mf.runtime, "lua") != 0) continue;
        if (!mf.app_id[0] || !mf.entry[0]) continue;

        am_app_t *e = add_bg(AM_KIND_LPK, mf.app_id, NULL);
        if (!e) break;
        if (!e->rt_id[0]) snprintf(e->rt_id, sizeof(e->rt_id), "%s", mf.app_id);
        if (!e->name_zh[0]) set_names(e, mf.label, mf.name, mf.app_id);
        if (!e->desc[0]) snprintf(e->desc, sizeof(e->desc), "%s", mf.description);
        snprintf(e->ver_local, sizeof(e->ver_local), "%s", mf.version);
        snprintf(e->build_local, sizeof(e->build_local), "%s", mf.build);
        snprintf(e->cat_zh, sizeof(e->cat_zh), "%s", cat_zh_of(mf.category));
        e->need_wifi = mf.has_permissions;
        if (!e->pkg[0]) snprintf(e->pkg, sizeof(e->pkg), "%s", nm);
        /* 未安装的包：只记"本地有安装包"，loc 保持 0（不是已安装） */
        if (!e->loc && !e->path_tf[0] && !e->path_flash[0])
            snprintf(e->entry, sizeof(e->entry), "%s", path);   /* 待安装源路径 */
    }
    closedir(d);
}

static void scan_lpk_packages(void)
{
    scan_lpk_pkg_dir(D_TF_APPS);
    scan_lpk_pkg_dir(D_TF_STORE_PKG);
    scan_lpk_pkg_dir(D_FLASH_ROOT);
}

/* ───────── 合并：服务器索引（只贡献远端字段 + 目录条目） ───────── */

static void merge_index(void)
{
    const int cnt = am_rstore_count();
    const bool ready = (cnt > 0);
    for (int r = 0; r < cnt; r++) {
        const am_rpkg_t *rp = am_rstore_get(r);
        if (!rp || !rp->name[0] || !rp->path[0]) continue;
        /* 规则 ①：**资源包不进应用列表**。
         *   索引里 type="asset"（字体/壁纸/游戏数据 zip）与 type="lpk"（仙剑这类
         *   打成 LPK 的游戏数据）都是**资源**：没有 app_id、没有运行入口，
         *   装完只是往 target 目录铺文件，给某个应用当素材用。
         *   ⚠ 曾经把 type="lpk" 当成"LPK 应用"，会把《仙剑奇侠传》的资源包
         *     错认成一个可启动应用（点开必然失败）。 */
        if (am_pkg_is_res(rp->type)) continue;
        const bool is_lua = (rp->type[0] == 0 || strcmp(rp->type, "lua") == 0);
        if (!is_lua) continue;

        char key[AM_CORE_KEY_LEN];
        am_core_make_key(AM_KIND_LUA, rp->name, key, sizeof(key));
        am_app_t *e = find_key_bg(key);
        if (!e) {
            /* ★ 目录条目叫「矢量地图」而卡上是 map.lua：身份仍按规则用文件名，
             *   但**包自己声明的名字**能精确对上目录名时，就是同一个应用。
             *   这里是精确相等（不是模糊相似），且只在"本地 Lua 还没对上目录"时用。 */
            for (int i = 0; i < s_bg_n; i++) {
                am_app_t *c = &s_bg[i];
                if (c->kind != AM_KIND_LUA || c->in_catalog) continue;
                if (c->name_zh[0] && strcmp(c->name_zh, rp->name) == 0) { e = c; break; }
            }
        }
        if (!e) e = add_bg(AM_KIND_LUA, rp->name, key);
        if (!e) continue;

        e->in_catalog = true;
        e->idx_ready  = ready;
        e->ridx       = (int16_t)r;
        snprintf(e->pkg_remote, sizeof(e->pkg_remote), "%s", rp->path);
        snprintf(e->ver_remote, sizeof(e->ver_remote), "%u", (unsigned)rp->ver);
        snprintf(e->build_remote, sizeof(e->build_remote), "%s", rp->build);
        e->mtime_remote = rp->mtime;
        e->crc_remote   = rp->crc;
        e->size_remote  = rp->size;
        /* ★ 2026-11-04 修复「下载最新版仍提示更新」：pkg 必须指向商店真实下载包文件名。
         *   预置包文件名（planeradar.lua）与商店条目名（航班雷达.lua）不同时，之前
         *   pkg 保留了预置文件名 ⇒ am_core_install_downloaded / am_core_update 按预置名
         *   去找下载落点 ⇒ 找不到 ⇒ 装不上 ⇒ 死循环。这里统一归一成索引 path 的文件名
         *   （商店下载/安装真正用的名字；新加入的条目此值与 "%s.lua" 等价）。 */
        {
            const char *slash = strrchr(rp->path, '/');
            const char *base = slash ? slash + 1 : rp->path;
            snprintf(e->pkg, sizeof(e->pkg), "%s", base);
        }
        if (!e->rt_id[0]) snprintf(e->rt_id, sizeof(e->rt_id), "lua:%s", rp->name);
        if (!e->name_zh[0]) set_names(e, rp->label[0] ? rp->label : rp->name, NULL, rp->name);
        if (!e->desc[0]) snprintf(e->desc, sizeof(e->desc), "%s", rp->desc);
        if (!e->cat_zh[0]) snprintf(e->cat_zh, sizeof(e->cat_zh), "%s", cat_zh_of(rp->cat));
        if (!e->icon_idx) e->icon_idx = cat_icon_of(e->cat_zh);   /* 必须在 cat 定下来之后 */
    }
    /* 索引未加载：所有条目都不能被判定成"服务器已删除" */
    if (!ready)
        for (int i = 0; i < s_bg_n; i++) s_bg[i].idx_ready = false;
}

/* ───────── 视图投影（不是 cast） ───────── */

/**
 * 把统一记录填成页面认识的 os_app_t。
 *
 * `view` 是 am_app_t 的**成员**（不是 reinterpret_cast）—— 页面拿到的每个指针都能用
 * am_core_from_view() 反查回记录，这是"页面只读核心、不再自己维护列表"的关键。
 *
 * rt_id 的分工（很重要，改错就丢桌面图标）：
 *   · 内置 C  → 原 id（"pal"/"calc"），因为 apps_registry.c 已用它注册过；
 *   · Lua     → "lua:<文件名>"，与 NVS mainmenu 里历史写入的值一致；
 *   · LPK     → manifest 的 app_id，与 am_lpkg 的 registry/桌面记录一致。
 * 三者都不带类型前缀 —— 类型前缀只属于 key（身份），不对外暴露。
 */
static void build_view(am_app_t *e)
{
    e->view = (os_app_t){0};
    e->view.id       = e->rt_id;
    e->view.label    = e->name_show;
    e->view.cat      = e->cat_zh[0] ? e->cat_zh : NULL;
    e->view.icon_idx = e->icon_idx;
    e->view.hidden   = true;
    e->view.page     = (e->kind == AM_KIND_BUILTIN) ? e->page : OS_PAGE_LUA;
    if (e->kind != AM_KIND_BUILTIN) e->view.requires = e->entry;  /* 文件没了就不显示 */
}

static void build_views(void)
{
    for (int i = 0; i < s_bg_n; i++) {
        am_app_t *e = &s_bg[i];
        /* rt_id 由各扫描器在**创建记录时**用原始字符串填好（文件名带扩展名 / app_id 原样），
         * 这里只兜底：万一漏填，就从 key 里剥掉类型前缀（会丢大小写，属最后手段）。 */
        if (!e->rt_id[0]) {
            const char *b = strchr(e->key, ':');
            if (e->kind == AM_KIND_LUA) snprintf(e->rt_id, sizeof(e->rt_id), "%s", e->key);
            else snprintf(e->rt_id, sizeof(e->rt_id), "%s", b ? b + 1 : e->key);
        }
        build_view(e);
    }
}

/* ───────── 扫描主流程 ───────── */

/* "卡装着并且能读"：/sdcard 存在 **且** apps 目录打得开。
 * 只 stat /sdcard 不够 —— 挂载/卸载的瞬间它会说谎，而那时 opendir 已经开始失败，
 * 于是扫描结果变成"一个 Lua 都没有"，界面上就是"所有 Lua 都变成下载"。 */
static bool tf_readable(void)
{
    if (!tf_avail()) return false;
    DIR *d = opendir(D_TF_APPS);
    if (!d) return false;
    closedir(d);
    return true;
}

static void core_scan(void)
{
    s_bg_n = 0;
    /* **一次扫描内只认一个"卡在不在"的快照**。
     * ⚠ 原来在扫描途中才逐个 opendir，而 SPIFFS/SD 挂载是异步的：
     *   开机那一刻卡可能正在挂载 ⇒ 前几个目录读得到、后几个读不到，
     *   于是"卡里 4 个 Lua 应用，只认出 2 个"，而且不会自愈。
     *   现在：不可用就整段跳过 TF；扫完把这次看到的卡状态记在 s_tf_seen 上，
     *   之后由 am_core_tick 里的探针比对 → 卡真挂上了会触发一次完整重扫。 */
    const bool tf = tf_avail();

    /* ★ 扫描失败**不能**覆盖上一次可靠结果。
     *   判据：卡在（tf=true）但 apps 目录读不了 ⇒ 这是 I/O 故障/挂载竞态，
     *   不是"用户把应用删了"。这种情况直接放弃本次发布，保留旧列表，稍后重试。
     *   （卡真的被拔掉时 tf=false，那条路是正常卸载语义：只隐藏、不改安装事实。） */
    const bool tf_ok = tf && tf_readable();
    if (tf && !tf_ok && s_ready) {
        ESP_LOGW(TAG, "TF 在但读不了 → 放弃本次扫描(保留上次结果), 稍后重试");
        s_scan_retry_at = esp_timer_get_time() + 3000000;   /* 3s 后重试 */
        return;                                             /* ⛔ 不置 s_bg_ready */
    }

    scan_builtin();
    scan_lua(tf_ok);
    scan_lpk_installed();
    if (tf_ok) scan_lpk_packages();
    merge_index();

    /* 存储可用性：TF 不可访问时，只存在于 TF 的条目标记 storage_ok=false
     * （**不改 loc** —— 安装事实以 registry/文件为准，拔卡不等于卸载） */
    for (int i = 0; i < s_bg_n; i++) {
        am_app_t *e = &s_bg[i];
        if (!tf && (e->loc & AM_LOC_TF) && !(e->loc & AM_LOC_FLASH)) e->storage_ok = false;
        else e->storage_ok = true;
        if (!e->loc && e->kind == AM_KIND_LPK && !e->entry[0]) e->valid = false;
    }
    /* 同 id 的内置 C 应用优先占应用中心（否则「仙剑」这类"内置页 + LPK 包"会出两条）：
     * LPK 条目仍在商店里正常显示/安装/更新，只是不重复进应用中心。 */
    for (int i = 0; i < s_bg_n; i++) {
        am_app_t *e = &s_bg[i];
        if (e->kind != AM_KIND_LPK) continue;
        const char *id = strchr(e->key, ':');
        id = id ? id + 1 : e->key;
        for (int j = 0; j < s_bg_n; j++) {
            if (s_bg[j].kind != AM_KIND_BUILTIN) continue;
            const char *bid = strchr(s_bg[j].key, ':');
            bid = bid ? bid + 1 : s_bg[j].key;
            if (strcmp(bid, id) == 0) { e->center_hide = true; break; }
        }
    }
    build_views();
    s_tf_seen = tf;          /* 记录"这次扫描看到的卡状态"，供 am_core_tick 的探针比对 */
    s_scan_retry_at = 0;     /* 扫描成功 ⇒ 清掉重试计划 */
    s_bg_ready = true;
}

static void core_task(void *arg)
{
    (void)arg;
    core_scan();
    s_busy = false;
    vTaskDelete(NULL);
}

static void core_kick(void)
{
    if (s_busy) return;
    s_busy = true;
    if (xTaskCreatePinnedToCore(core_task, "amcore", 8192, NULL, 3, NULL, 0) != pdPASS) {
        s_busy = false;
        ESP_LOGW(TAG, "扫描任务创建失败，下次再试");
    }
}

void am_core_invalidate(void) { s_dirty = true; if (!s_busy) core_kick(); }

void am_core_boot(void) { s_dirty = true; core_kick(); }

bool am_core_tick(void)
{
    bool changed = false;

    /* TF 插拔自愈：每 2 秒探一次 /sdcard 是否可访问，与**上次扫描时看到的状态**比对。
     * ⚠ s_tf_seen 只由 core_scan 写，这里绝不自己"顺手同步" ——
     *   否则开机那次（扫描时卡还没挂上）会被探针悄悄消化掉，
     *   卡挂上后永远不再重扫，表现为"卡里 4 个应用只认出 2 个"。
     * 拔卡不卸载：loc / registry 一律不动，只是那几条暂时不可见。 */
    {
        static int64_t s_tf_probe_us = 0;
        const int64_t now = esp_timer_get_time();
        if (now - s_tf_probe_us > 2000000) {
            s_tf_probe_us = now;
            if (s_ready && tf_avail() != s_tf_seen) {
                ESP_LOGI(TAG, "TF 状态变化 → 重扫应用列表");
                am_core_invalidate();
            }
        }
    }

    /* 上次扫描失败（卡在读）→ 到点重试。用时间戳退避，避免每帧撞一次 I/O。 */
    if (s_scan_retry_at && !s_busy && esp_timer_get_time() >= s_scan_retry_at) {
        s_scan_retry_at = 0;
        s_dirty = true;
        core_kick();
    }

    /* 定向修改（卸载单条）也要让页面重建一次，但**不重扫**：
     * 这是"只影响被卸载的那一个"的关键 —— 别的应用在这里一个字节都没动。 */
    if (s_touch) { s_touch = false; changed = true; }

    /* ★ 消费后台卸载投递的 key：把这一条改成"未安装"。
     *   之前 am_core_remove() 只把 key 存进 s_pending_rm 却没人读，
     *   于是"卸载后立刻变按钮"完全靠后台重扫 —— 重扫要等扫描任务跑完，
     *   页面在此期间拿着**旧指针**渲染，说明/按钮都是过期的。
     *   现在由 UI 线程在这里直接落地：只改这一条，不动别人。 */
    if (s_pending_rm_set) {
        s_pending_rm_set = false;              /* 先清标志：防止处理期间又被投递覆盖 */
        char key[AM_CORE_KEY_LEN];
        snprintf(key, sizeof(key), "%s", s_pending_rm);
        s_pending_rm[0] = 0;
        apply_removed(key);
        s_touch = true;
        changed = true;
    }

    if (s_bg_ready) {
        s_bg_ready = false;
        /* 交换读写缓冲：旧读缓冲整份保留，s_list 里的指针不会指向"换了内容"的条目 */
        am_app_t *tmp = s_app;
        s_app  = s_bg;
        s_bg   = tmp;
        s_n    = s_bg_n;
        /* 视图里的指针指向**本结构自己的字段**，交换后必须按新地址重填一次。 */
        for (int i = 0; i < s_n; i++) build_view(&s_app[i]);
        s_ready = true;
        s_dirty = false;
        changed = true;
        s_gen++;        /* ★ 读侧换代 ⇒ 代次 +1（页面据此强制重建列表） */

        /* ★ 注册到 os_app 注册表（主菜单/桌面图标的唯一来源）。
         *   内置 C 由 apps_registry.c 注册，**这里跳过**，否则会多出一份带前缀的重复项。
         *   同 id 覆盖语义 ⇒ Lua/LPK 重新扫出来时会自然刷新指针，不会越积越多。 */
        for (int i = 0; i < s_n; i++) {
            if (s_app[i].kind == AM_KIND_BUILTIN) continue;
            if (!s_app[i].valid) continue;
            os_app_register(&s_app[i].view);
        }
    } else if (s_dirty && !s_busy) {
        core_kick();
    }
    return changed;
}

bool am_core_ready(void) { return s_ready; }

/** 读侧数据代次：每次换代/定向改动 +1。
 *  页面用它判断"手里这份列表还作数吗" —— 页面列表存的是 &s_app[i].view
 *  裸指针，核心换代后旧指针全部失效，必须重建才能取到新的说明/按钮。 */
uint32_t am_core_gen(void) { return s_gen; }

/* ───────── 读接口 ───────── */

int am_core_count(void) { return s_n; }

const am_app_t *am_core_at(int i) { return (i >= 0 && i < s_n) ? &s_app[i] : NULL; }

const char *am_core_key_of(const am_app_t *a) { return a ? a->key : ""; }

const char *am_core_pkg_name_of(const am_app_t *a) { return a ? a->pkg : ""; }

const char *am_core_label_of(const am_app_t *a) { return a ? a->name_show : ""; }

bool am_core_need_wifi_of(const am_app_t *a) { return a ? a->need_wifi : false; }

/* 指针 → 记录。只认本模块数组里的 view 成员，其它指针一律 NULL ——
 * 这样页面把"内置注册表原件"误传进来时会被当成"不是统一核心管理的条目"，
 * 而不会退化成对野指针的解读。 */
const am_app_t *am_core_from_view(const os_app_t *view)
{
    if (!view) return NULL;
    for (int i = 0; i < s_n; i++)
        if (&s_app[i].view == view) return &s_app[i];
    return NULL;
}

const am_app_t *am_core_find(const char *key)
{
    if (!key || !key[0]) return NULL;
    for (int i = 0; i < s_n; i++) if (strcmp(s_app[i].key, key) == 0) return &s_app[i];
    return NULL;
}

static bool center_ok(const am_app_t *e)
{
    if (!e) return false;
    return am_core_center_ok(e->valid, e->kind, e->loc, e->storage_ok, e->center_hide);
}

static bool store_ok(const am_app_t *e)
{
    if (!e) return false;
    return am_core_store_ok(e->valid, e->kind);
}

int am_core_count_center(void)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) if (center_ok(&s_app[i])) n++;
    return n;
}

const am_app_t *am_core_center_at(int idx)
{
    int n = 0;
    for (int i = 0; i < s_n; i++)
        if (center_ok(&s_app[i]) && n++ == idx) return &s_app[i];
    return NULL;
}

int am_core_count_store(void)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) if (store_ok(&s_app[i])) n++;
    return n;
}

const am_app_t *am_core_store_at(int idx)
{
    int n = 0;
    for (int i = 0; i < s_n; i++)
        if (store_ok(&s_app[i]) && n++ == idx) return &s_app[i];
    return NULL;
}

/* ───────── 统一状态 ─────────
 *
 * 判据只有**包内声明的版本**和**索引声明的版本**，不再读 .stm 边车记录：
 *   · 本地版本来自包自己（LAPP 的 build=/ver=、LPK 的 manifest / registry）；
 *   · 远端版本来自 index.json 的 build/ver；
 *   · 两者都是"这个应用是什么版本"的**声明**，不需要在安装时另外记账。
 * 这样"重装/换个位置装/手动拷进来"都能立刻得出正确结论，
 * 也不会再出现"更新完仍显示更新"（那正是拿下载时间冒充版本导致的）。
 * 具体比较逻辑在 am_core_logic.c 的 am_core_decide()，此处只负责取字段。 */
am_st_t am_core_state_of(const am_app_t *e)
{
    if (!e || !e->valid) return AM_ST_NONE;
    if (e->kind == AM_KIND_BUILTIN) return AM_ST_INSTALLED;

    const bool installed = (e->loc & (AM_LOC_FLASH | AM_LOC_TF)) != 0;
    return am_core_decide(installed, e->in_catalog, e->idx_ready,
                          e->build_local, e->ver_local,
                          e->build_remote, e->ver_remote);
}

bool am_core_has_local_pkg(const am_app_t *e)
{
    if (!e || e->kind != AM_KIND_LPK) return false;
    if (e->loc) return false;                      /* 已安装，不是"待安装包" */
    return e->entry[0] && exists(e->entry);
}

const char *am_core_btn_of(const am_app_t *e)
{
    if (!e) return "";
    am_st_t st = am_core_state_of(e);
    if (st != AM_ST_NONE) return am_st_btn(st);

    /* 未安装：商店只允许「下载 / 安装 / 更新」三种之一。
     *   · 本地有 .lpk 安装包 + 服务器有更新版 → 更新（优先装服务器最新版）
     *   · 本地有 .lpk 安装包            → 安装（用本地包，不必联网）
     *   · 本地没有                      → 下载 */
    if (am_core_has_local_pkg(e))
        return am_core_remote_newer(e->build_local, e->ver_local,
                                    e->build_remote, e->ver_remote) ? "更新" : "安装";
    return am_st_btn(AM_ST_NONE);                  /* 「下载」 */
}

/* ════════════════════════════════════════════════════════════════════════
 * 动作：安装 / 更新 / 卸载 —— **唯一入口**
 * ------------------------------------------------------------------------
 * 原则（linit 2026-11-03 定稿 ④）：
 *   LPK 一律复用 am_lpkg_install() / am_lpkg_uninstall()（已验证的实现），
 *   本模块**不自己解包、不自己删树**，只负责挑对入口、清干净副本、刷状态。
 * ════════════════════════════════════════════════════════════════════════ */

static void setwhy(char *why, size_t cap, const char *msg)
{
    if (why && cap) snprintf(why, cap, "%s", msg ? msg : "");
}

static const char *base_of(const char *p)
{
    if (!p || !p[0]) return "";
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static bool copy_file(const char *src, const char *dst)
{
    if (!exists(src)) return false;
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[512];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(dst);
    return ok;
}

/* 安装目标目录：TF 卡优先（内置 apps 分区只有 4MB），没卡才退回内置。
 * 子目录按声明分类定：游戏/程序/软件 → app，运维/工具 → ops，其余 → app。
 * ⚠ 与旧实现一致：**新装一律优先 TF**，但"已存在于内置"的应用更新时留在内置。 */
static void target_dir(const am_app_t *e, char *out, size_t cap)
{
    const char *sub = (strcmp(e->cat_zh, "运维") == 0) ? "ops" : "app";
    /* 已在内置的（老数据/用户移进去的）继续留内置；否则新装一律优先 TF
     * —— 内置 apps 分区只有 4MB，卡上几十 G，没理由挤内置。无卡时退回内置。 */
    const bool use_flash = (e->path_flash[0] != 0) || !tf_avail();
    snprintf(out, cap, "%s/%s", use_flash ? D_FLASH_APPS : D_TF_APPS, sub);
}

/* ★ 2026-11-03 linit: **清掉同一应用的旧副本**（文件名不同、包内 name 相同）。
 *
 *   为什么需要：预置包叫 map.lua（包内 name=矢量地图），商店发布的是 矢量地图.lua。
 *   身份按**规范化文件名**算 ⇒ 两者是不同 key ⇒ 核心认为是两个应用。
 *   于是下载安装新版后，旧记录还在（build 更旧）⇒ 界面一直提示「更新」。
 *
 *   判据：**包内 LAPP 头声明的 name 与本应用精确相等**（不是模糊匹配，
 *   也不是按文件名猜）⇒ 确实是同一个应用，只是文件换了名字。
 *
 *   只删 .lua 文件 + 它的 .stm 边车；绝不碰别的应用。
 *   keep_path = 刚装好的那个文件，不能删。 */
static void purge_same_name_others(const am_app_t *e, const char *keep_path)
{
    if (!e || e->kind != AM_KIND_LUA || !e->name_zh[0]) return;

    /* 要扫的目录: Flash 与 TF 的应用目录 + 根目录 + 待装目录 */
    static const char *const dirs[] = {
        D_FLASH_APPS "/app", D_FLASH_APPS "/ops", D_FLASH_APPS "/engine", D_FLASH_ROOT,
        D_TF_APPS "/app",   D_TF_APPS "/ops",   D_TF_APPS "/engine",   D_TF_APPS,
    };

    for (size_t di = 0; di < sizeof(dirs) / sizeof(dirs[0]); di++) {
        DIR *d = opendir(dirs[di]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name;
            if (nm[0] == '.') continue;
            size_t len = strlen(nm);
            if (len <= 5 || strcasecmp(nm + len - 4, ".lua") != 0) continue;

            char path[AM_CORE_PATH_LEN];
            snprintf(path, sizeof(path), "%s/%s", dirs[di], nm);
            if (keep_path && strcmp(path, keep_path) == 0) continue;  /* 刚装好的, 留着 */

            /* 包内 name 与本应用相同 ⇒ 是旧副本 */
            apppack_meta_t m;
            if (apppack_parse(path, &m) != ESP_OK) continue;
            if (!m.name[0] || strcmp(m.name, e->name_zh) != 0) continue;

            if (remove(path) == 0) {
                char stm[AM_CORE_PATH_LEN + 8];
                snprintf(stm, sizeof(stm), "%s.stm", path);
                remove(stm);
                ESP_LOGI(TAG, "删除同应用旧副本: %s (name=%s)", path, m.name);
            }
        }
        closedir(d);
    }
}

/* 把 src 安装成该记录。Lua = 复制单文件（先 .new 再换名，失败保留旧版）；
 * LPK = 交给 am_lpkg_install_{src2}（解包/校验/registry 全在那边）。 */
static esp_err_t do_install(const am_app_t *e, const char *src,
                           char *why, size_t why_cap)
{
    if (!src || !src[0] || !exists(src)) {
        setwhy(why, why_cap, "安装包不存在");
        return ESP_ERR_NOT_FOUND;
    }
    if (e->kind == AM_KIND_LPK) {
        esp_err_t r = am_lpkg_install_src2(src, e->ver_remote[0] ? e->ver_remote : NULL,
                                          e->build_remote[0] ? e->build_remote : NULL,
                                          e->mtime_remote, e->crc_remote, e->size_remote,
                                          why, why_cap);
        if (r == ESP_OK) am_core_invalidate();
        return r;
    }

    const char *fname = e->pkg[0] ? e->pkg : base_of(src);
    char dir[128];
    target_dir(e, dir, sizeof(dir));
    char dst[AM_CORE_PATH_LEN], tmp[AM_CORE_PATH_LEN + 8], bak[AM_CORE_PATH_LEN + 8];
    snprintf(dst, sizeof(dst), "%s/%s", dir, fname);
    snprintf(tmp, sizeof(tmp), "%s.new", dst);
    snprintf(bak, sizeof(bak), "%s.old", dst);
    mkdir(D_FLASH_APPS, 0755);
    mkdir(dir, 0755);
    remove(tmp); remove(bak);
    if (!copy_file(src, tmp)) { remove(tmp); setwhy(why, why_cap, "复制失败（空间不足？）"); return ESP_FAIL; }
    const bool had = exists(dst);
    if (had && rename(dst, bak) != 0) { remove(tmp); setwhy(why, why_cap, "旧版本挪不动"); return ESP_FAIL; }
    if (rename(tmp, dst) != 0) {
        remove(tmp);
        if (had) rename(bak, dst);                    /* 回滚到旧版，绝不把应用搞丢 */
        setwhy(why, why_cap, "写入失败，已保留旧版本");
        return ESP_FAIL;
    }
    if (had) remove(bak);
    /* 另一侧还留着旧副本 → 删掉（同一个应用两份会启动到旧的那份） */
    const char *other = (dir[0] == '/') && (strncmp(dir, D_TF_APPS, strlen(D_TF_APPS)) == 0)
                      ? e->path_flash : e->path_tf;
    if (other[0] && strcmp(other, dst) != 0) remove(other);

    /* ★ 2026-11-03 linit: **删掉同应用的旧副本**（文件名不同、但包内 name 相同）。
     *   现场：预置包叫 map.lua（name=矢量地图），商店发布的是 矢量地图.lua。
     *   两者**文件名不同 ⇒ 身份 key 不同 ⇒ 核心认为是两个应用**，
     *   于是下载安装新版后，旧的那条记录还在，build 更旧 ⇒ 一直提示「更新」，
     *   用户看到的现象就是"下了最新版还是提示更新"。
     *   这里按**包内声明的 name** 精确相等来找旧副本（不是模糊匹配），
     *   只删文件不同名、但确实是同一个应用的那一份。 */
    purge_same_name_others(e, dst);
    am_core_invalidate();
    return ESP_OK;
}

esp_err_t am_core_install(const char *key, char *why, size_t why_cap)
{
    if (why && why_cap) why[0] = 0;
    const am_app_t *e = am_core_find(key);
    if (!e) { setwhy(why, why_cap, "找不到该应用"); return ESP_ERR_NOT_FOUND; }
    if (e->kind == AM_KIND_BUILTIN) { setwhy(why, why_cap, "内置应用无需安装"); return ESP_ERR_INVALID_STATE; }
    if (e->loc) { setwhy(why, why_cap, "已安装"); return ESP_ERR_INVALID_STATE; }
    return do_install(e, e->entry, why, why_cap);
}

esp_err_t am_core_install_downloaded(const char *fname, char *why, size_t why_cap)
{
    if (why && why_cap) why[0] = 0;
    if (!fname || !fname[0]) { setwhy(why, why_cap, "文件名为空"); return ESP_ERR_INVALID_ARG; }

    /* 下载落点：TF 优先，其次内置根 */
    char src[AM_CORE_PATH_LEN];
    snprintf(src, sizeof(src), "%s/%s", D_TF_STORE_PKG, fname);
    if (!exists(src)) snprintf(src, sizeof(src), "%s/%s", D_FLASH_ROOT, fname);
    if (!exists(src)) { setwhy(why, why_cap, "下载的包找不到"); return ESP_ERR_NOT_FOUND; }

    /* 按包名找记录：先精确 pkg，再按主干比（索引 name / 文件名主干） */
    const am_app_t *hit = NULL;
    for (int i = 0; i < s_n; i++) {
        if (s_app[i].pkg[0] && strcmp(s_app[i].pkg, fname) == 0) { hit = &s_app[i]; break; }
    }
    if (!hit) {
        char stem[56];
        snprintf(stem, sizeof(stem), "%s", fname);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        for (int i = 0; i < s_n; i++) {
            const am_app_t *e = &s_app[i];
            char s2[56];
            snprintf(s2, sizeof(s2), "%s", e->pkg[0] ? e->pkg : e->key);
            char *d2 = strrchr(s2, '.');
            if (d2) *d2 = 0;
            if (strcmp(s2, stem) == 0) { hit = e; break; }
        }
    }
    if (!hit) {
        /* 兜底：按包内声明的 name 精确匹配（文件名不同、但确属同一应用，
         *   例如预置 planeradar.lua 与商店 航班雷达.lua）。避免"下载完成了却装不上"。 */
        apppack_meta_t dm;
        if (apppack_parse(src, &dm) == ESP_OK && dm.name[0]) {
            for (int i = 0; i < s_n; i++) {
                if (strcmp(s_app[i].name_zh, dm.name) == 0) { hit = &s_app[i]; break; }
            }
        }
    }
    if (!hit) { setwhy(why, why_cap, "下载完成但这个包无法识别"); return ESP_ERR_NOT_FOUND; }
    return do_install(hit, src, why, why_cap);
}

esp_err_t am_core_update(const char *key, char *why, size_t why_cap)
{
    if (why && why_cap) why[0] = 0;
    const am_app_t *e = am_core_find(key);
    if (!e) { setwhy(why, why_cap, "找不到该应用"); return ESP_ERR_NOT_FOUND; }
    if (am_core_state_of(e) != AM_ST_UPDATE) { setwhy(why, why_cap, "当前没有可用更新"); return ESP_ERR_INVALID_STATE; }

    /* 更新包由下载器先落到 store_pkg（本模块不联网） */
    char pkg[AM_CORE_PATH_LEN];
    snprintf(pkg, sizeof(pkg), "%s/%s", D_TF_STORE_PKG, e->pkg[0] ? e->pkg : base_of(e->entry));
    if (!exists(pkg)) snprintf(pkg, sizeof(pkg), "%s/%s", D_FLASH_ROOT, e->pkg[0] ? e->pkg : base_of(e->entry));
    if (!exists(pkg)) { setwhy(why, why_cap, "更新包尚未下载"); return ESP_ERR_NOT_FOUND; }
    return do_install(e, pkg, why, why_cap);
}

/* 卸载成功后的**定向**更新：只动这一条。
 * 为什么不靠重扫：一次扫描失败/卡一时读不到，就会把**所有** TF 应用判成未安装
 * —— 那正是"卸载一个 Lua，全部变下载"的现场。重扫只负责校正，不负责让按钮变对。
 * ⚠ 会写读侧缓冲，**只能在 UI 线程调**（后台卸载走 s_pending_rm，由 tick 代劳）。 */
static void apply_removed(const char *key)
{
    for (int i = 0; i < s_n; i++) {
        am_app_t *e = &s_app[i];
        if (strcmp(e->key, key) != 0) continue;
        e->loc = AM_LOC_NONE;
        e->path_flash[0] = 0;
        e->path_tf[0] = 0;
        e->storage_ok = true;
        if (e->kind == AM_KIND_LPK) {
            e->valid = false;       /* 安装包已装/已删，这条不再代表"可安装的包" */
        } else {
            e->entry[0] = 0;
            /* 本地独立应用（服务器没有它）卸完就该整条消失；
             * 商店里有的应用保留一行，按钮变「下载」。 */
            if (!e->in_catalog) e->valid = false;
        }
        build_view(e);
        break;
    }
    /* 读侧内容变了 ⇒ 代次 +1，页面据此重建（否则它手里还是旧指针/旧文案） */
    s_gen++;
}

void am_core_mark_removed(const char *key)
{
    if (!key || !key[0]) return;
    apply_removed(key);
    s_touch = true;             /* 让下一次 tick 报"有变化"，页面立刻重建 */
}

/* 删掉一条 Lua 应用的**所有已确认副本**：Flash、TF、两个待装目录、扫描命中的源文件，
 * 外加它的 `.stm` 伴随记录（否则"卸载后还剩个文件"）。
 * 返回 OK = 至少删掉一个；一个都没删到 ⇒ NOT_FOUND（绝不假装卸载成功）。 */
static esp_err_t remove_lua_files(const am_app_t *e, char *why, size_t why_cap)
{
    char paths[6][AM_CORE_PATH_LEN];
    int np = 0;
    const char *fname = e->pkg[0] ? e->pkg : base_of(e->entry);

    if (e->path_flash[0]) snprintf(paths[np++], AM_CORE_PATH_LEN, "%s", e->path_flash);
    if (e->path_tf[0])    snprintf(paths[np++], AM_CORE_PATH_LEN, "%s", e->path_tf);
    if (e->entry[0])      snprintf(paths[np++], AM_CORE_PATH_LEN, "%s", e->entry);
    if (fname[0]) {
        snprintf(paths[np++], AM_CORE_PATH_LEN, "%s/%s", D_TF_STORE_PKG, fname);
        snprintf(paths[np++], AM_CORE_PATH_LEN, "%s/%s", D_FLASH_ROOT,   fname);
    }

    int n = 0;
    for (int i = 0; i < np; i++) {
        if (!paths[i][0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) if (strcmp(paths[i], paths[j]) == 0) { dup = true; break; }
        if (dup) continue;                       /* 同一路径不重复删（entry 常等于 path_tf） */
        if (remove(paths[i]) == 0) {
            n++;
            char stm[AM_CORE_PATH_LEN + 8];
            snprintf(stm, sizeof(stm), "%s.stm", paths[i]);
            remove(stm);                         /* 安装记录边车，属于这个应用 */
        }
    }
    /* ★ 2026-11-03 linit: **卸载也要删干净同名旧副本**。
     *   预置包 map.lua(name=矢量地图) 与商店包 矢量地图.lua 是同一个应用、
     *   但文件名不同 ⇒ 两条记录。卸掉其中一条，另一条还在 ⇒ 卸载不干净。
     *   这里按包内 name 精确相等，把同名的另一份也删掉（keep_path=NULL ⇒ 全删）。 */
    if (e->kind == AM_KIND_LUA) {
        purge_same_name_others(e, NULL);
        n++;   /* 同名旧副本已清，不因"没删到主文件"而误报失败 */
    }
    if (!n) { setwhy(why, why_cap, "文件没找到（可能已被删除）"); return ESP_ERR_NOT_FOUND; }
    return ESP_OK;
}

esp_err_t am_core_remove(const char *key, bool keep_data, char *why, size_t why_cap)
{
    if (why && why_cap) why[0] = 0;
    const am_app_t *e = am_core_find(key);
    if (!e) { setwhy(why, why_cap, "找不到该应用"); return ESP_ERR_NOT_FOUND; }
    if (e->kind == AM_KIND_BUILTIN) { setwhy(why, why_cap, "内置应用不可卸载"); return ESP_ERR_INVALID_STATE; }

    esp_err_t r;
    if (e->kind == AM_KIND_LPK) {
        const char *id = strchr(e->key, ':');
        id = id ? id + 1 : e->key;
        r = am_lpkg_uninstall(id, keep_data, why, why_cap);   /* 复用正式实现 */
    } else {
        r = remove_lua_files(e, why, why_cap);
    }
    if (r == ESP_OK) {
        /* 定向改这一条 + 后台重扫校正。**绝不**用"整表重来"表达一次卸载。
         * ⚠ 本函数跑在**后台卸载任务**里，不能碰读侧缓冲 ——
         *   只投递 key，由 UI 线程的 am_core_tick 落地。
         *   顺序：先写字符串再置标志，UI 侧看到标志才保证 key 已完整。 */
        snprintf(s_pending_rm, sizeof(s_pending_rm), "%s", key);
        s_pending_rm_set = true;
        am_core_invalidate();
    }
    return r;
}

esp_err_t am_core_move_to(const char *key, bool to_flash, char *why, size_t why_cap)
{
    if (why && why_cap) why[0] = 0;
    const am_app_t *e = am_core_find(key);
    if (!e) { setwhy(why, why_cap, "找不到该应用"); return ESP_ERR_NOT_FOUND; }
    if (e->kind != AM_KIND_LUA) { setwhy(why, why_cap, "只支持 Lua 应用搬家"); return ESP_ERR_INVALID_STATE; }

    const char *src = to_flash ? e->path_tf : e->path_flash;
    if (!src[0] || !exists(src)) {
        setwhy(why, why_cap, to_flash ? "卡上没有这个应用" : "内置没有这个应用");
        return ESP_ERR_NOT_FOUND;
    }
    const char *fname = e->pkg[0] ? e->pkg : base_of(src);
    const char *sub = (strcmp(e->cat_zh, "运维") == 0) ? "ops" : "app";
    char dir[128], dst[AM_CORE_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/%s", to_flash ? D_FLASH_APPS : D_TF_APPS, sub);
    snprintf(dst, sizeof(dst), "%s/%s", dir, fname);

    mkdir(dir, 0755);
    if (!copy_file(src, dst)) { remove(dst); setwhy(why, why_cap, "复制失败（空间不足？）"); return ESP_FAIL; }
    struct stat sa, sb;
    if (stat(src, &sa) != 0 || stat(dst, &sb) != 0 || sa.st_size != sb.st_size) {
        remove(dst);                                     /* 校验不符就别删源 */
        setwhy(why, why_cap, "复制校验不符，原文件已保留");
        return ESP_FAIL;
    }
    remove(src);                                         /* 校验通过才删源 */
    am_core_invalidate();
    return ESP_OK;
}
