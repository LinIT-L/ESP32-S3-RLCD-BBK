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
    char     id[56];     /* pkg:xxx.lua (fname 最长 48 → "pkg:" 前缀 + NUL 要塞得下) */
    char     label[48];  /* 中文名 */
    char     cat[16];    /* 分类文件夹 engine/game/app */
    char     src[192];   /* 源完整路径 */
    char     dst[192];   /* 目标完整路径(内置固件) */
    char     dst_sd[192];/* 目标完整路径(TF卡) */
    int      pref;       /* 推荐安装目标: 0=内置固件 1=TF卡 */
    char     fname[48];  /* 文件名 xxx.lua */
    char     desc[96];   /* LAPP 简介 (无打包则空) */
    char     cat_zh[16]; /* ★ 中文分类(游戏/运维/软件...) —— app.cat 指向这里, 不能用局部数组(悬空) */
    uint8_t  kind;       /* 0=本地已有包(可安装)  1=云端包(需先下载) —— 2026-10-04 */
    int16_t  ridx;       /* kind=1 时: 对应 am_rstore_get() 的下标 */
} am_pkg_entry_t;
static EXT_RAM_BSS_ATTR am_pkg_entry_t s_pkg[AM_PKG_MAX];
static int s_pkg_n = 0;
static volatile int s_pkg_scanned = -1;   /* 0=已扫描 (数据可用)  -1=未扫/扫描中 (后台任务写入) */
static volatile bool s_pkg_busy  = false;   /* 后台扫描任务进行中 */
static volatile bool s_pkg_done  = false;   /* 后台扫描已完成 (UI 线程取货) */
static bool s_pkg_inst[AM_PKG_MAX]; /* 每包安装状态缓存: 渲染用, 避免每帧 fopen(SD/SPIFFS 慢)致商店卡顿 */

/* ---- 云端包视图 (2026-10-04) ----
 * 索引里的包**不直接进 s_pkg[]** —— s_pkg[] 是后台扫描任务的产物, 而索引是另一个
 * 后台任务在稍后才送到的, 两边写同一个数组必炸。这里单独开一张"只读视图",
 * 由 UI 线程在 am_store_tick() 里按索引重建 (`kind=1`), 渲染/点击都当成普通条目用。
 * 同一个包一旦被下载到商店源目录, 下次扫描就进了 s_pkg[], 本视图自动把它剔掉。 */
#define AM_RVIEW_MAX 40
static EXT_RAM_BSS_ATTR am_pkg_entry_t s_rview[AM_RVIEW_MAX];
static int  s_rview_n = 0;
static int  s_rview_synced = -1;    /* 已按 am_rstore_count()=几 建过 (-1=还没建) */

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
/* 商店列表要的是 main_icons 索引, 而 LAPP 头里带的是 64x64 位图(应用管家用) ——
 * 两者没有反查关系。原实现把所有包硬写成 ICON_MINI_GAMES(迷你游戏), 看着像 bug。
 * 这里给已知包名一个小映射, 未知包沿用默认。 */
/* ★★★ 2026-10-17 linit: 按【分类】给默认图标 —— 之前没有专属图标的包一律回退
 *   ICON_MINI_GAMES(迷你游戏) ⇒ 商店里所有云包都长一个游戏图标, 看着毫无分类。
 *   engine(游戏/引擎)=迷你游戏 · ops(运维)=终端 · app(软件)=应用中心。 */
/* ★★★ 2026-10-17 linit: **每个分类一个默认图标** —— 应用自己没带图标时用它。
 *   五类全用现成图标(实测都合适):
 *     游戏 → 迷你游戏 · 软件 → 应用中心 · 标准 → 设置 · 运维 → 终端 · 学习 → 阅读器 */
static int am_store_cat_icon(const char *cat) {
    if (!cat || !cat[0])            return ICON_APP_MGR;
    if (strcmp(cat, "游戏") == 0)   return ICON_MINI_GAMES;
    if (strcmp(cat, "运维") == 0)   return ICON_TERMINAL;
    if (strcmp(cat, "标准") == 0)   return ICON_SETTINGS;
    if (strcmp(cat, "学习") == 0)   return ICON_BOOK;
    if (strcmp(cat, "engine") == 0) return ICON_MINI_GAMES;   /* 兼容旧目录名 */
    if (strcmp(cat, "ops") == 0)    return ICON_TERMINAL;
    return ICON_APP_MGR;                                      /* 软件/其它 */
}
static int am_store_icon_of(const char *fname, const char *cat) {
    static const struct { const char *f; int ic; } T[] = {
        { "biossim.lua", ICON_BIOSSIM },          /* BIOS 装机模拟器 */
        { "planeradar.lua", ICON_PLANERADAR },    /* 航班雷达 */
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (strcmp(T[i].f, fname) == 0) return T[i].ic;
    return am_store_cat_icon(cat);                /* ★ 无专属图标 ⇒ 用分类默认图标 */
}
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
    /* ★★★ 2026-10-17 linit: 除了"待下载"目录, **也扫已安装目录** ——
     *   这样本地已装的应用(哪怕云端索引里没有)也会出现在商店里, 方便直接卸载。
     *   b<2 = 待下载源(store 根 / SD store_pkg); b>=2 = 已安装目录(/store/apps/{cat})。 */
    const char *bases[5] = { AM_STORE_SRC1, AM_STORE_SRC2,
                             AM_STORE_DST "/app", AM_STORE_DST "/engine", AM_STORE_DST "/ops" };
    for (int b = 0; b < 5; b++) {
        DIR *d = opendir(bases[b]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            if (nm[0] == '.') continue;
            /* ★★ 只取**直接子项** (2026-10-02 定案, 这是"两个 BIOS 模拟器/可重复安装"的根因):
             *   ESP-IDF 的 SPIFFS VFS `readdir` 并不会像 FAT 那样逐层展开 ——
             *   `esp_spiffs.c: vfs_spiffs_readdir_r()` 只剥掉一层前缀, 之后**原样返回剩余路径**。
             *   实测 `opendir("/store")` 会同时吐出 "apps"、"apps/app"、"apps/app/biossim.lua"
             *   —— 后两个名字里带 '/'。旧代码只用"长度>5 且以 .lua 结尾"过滤 →
             *   **预装副本 apps/app/xxx.lua 被当成"商店待装包"** → 商店里出现同名第二条,
             *   点"安装"又往 /apps/app/ 复制一份 → 想装几次就重复几份。
             *   (am_lua.c 扫 /store/apps/{cat} 不受影响: 它剥前缀后正好只剩文件名。) */
            if (strchr(nm, '/')) continue;
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
                /* ★★★ 2026-10-17 linit: `/sdcard/store_pkg` 是**商店自己的下载目录**,
                 *   只显示云端索引里存在的包 —— 旧的英文名残留文件(改名前的下载)自动隐藏,
                 *   商店里不会再出现"英文名 + 中文名"两份。/store(SPIFFS 根)放宽, 允许用户自己放。 */
                /* LAPP 打包则用包内 name/desc (可中文), 否则文件名 */
                char spath[192];
                snprintf(spath, sizeof(spath), "%s/%s", bases[b], nm);
                /* ★ 2026-10-17 linit: 商店自己的下载目录里, 索引中没有的残留包**直接删除**
                 *   (改名/过期留下的旧文件), 不再隐藏。手动往 /store 放的不受影响。 */
                if (b == 1) {
                    bool known = false;
                    int rn = am_rstore_count();
                    for (int r = 0; r < rn; r++) {
                        const am_rpkg_t *rp = am_rstore_get(r);
                        if (rp && strcmp(rp->name, nb) == 0) { known = true; break; }
                    }
                    if (!known) {
                        ESP_LOGW("AMSTORE", "删除残留包: %s", spath);
                        remove(spath);
                        continue;
                    }
                }
                apppack_meta_t meta;
                /* ★★★ 2026-10-17 linit 定: **显示名以文件名(去 .lua)为准** —— 游戏/模拟器习惯用文件名,
                 *   LAPP 里的 name 仅作兜底(文件名为空时)。分类/图标仍读 LAPP 头。 */
                apppack_parse(spath, &meta);
                snprintf(s_pkg[k].label, sizeof(s_pkg[k].label), "%.*s", (int)len - 4, nm);
                if (!s_pkg[k].label[0] && meta.name[0])
                    snprintf(s_pkg[k].label, sizeof(s_pkg[k].label), "%s", meta.name);
                snprintf(s_pkg[k].desc, sizeof(s_pkg[k].desc), "%s", meta.desc);
                /* ★★★ 2026-10-17: 本地包也按【云端索引里的 cat】分类 —— 之前写死 "app" +
                 *   app.cat=NULL ⇒ 所有包都显示在"软件", 且在游戏/运维子分类里看不到。
                 *   索引里查不到同名包才回退 app。 */
                /* ★★★ 分类**优先读应用自带的 LAPP 头**(--@app cat=), 这是唯一真相源;
                 *   包头里没有分类时, 才用云端索引里同名包的 cat 兜底, 再不行归"软件"。 */
                /* ★★★ 2026-10-17 修: **分类优先取云端索引**(索引里的 cat 由 store_build 从各应用
                 *   LAPP 头生成, 已验证正确 ✓); `meta.cat` 作兜底 —— 实测 apppack_parse 没填 cat,
                 *   导致所有包都掉到默认"软件"。 */
                char icat[16];
                snprintf(icat, sizeof(icat), "%s", "");
                {
                    char bn[48];
                    snprintf(bn, sizeof(bn), "%.*s", (int)(len - 4), nm);
                    int rn = am_rstore_count();
                    for (int r = 0; r < rn; r++) {
                        const am_rpkg_t *rp = am_rstore_get(r);
                        if (rp && strcmp(rp->name, bn) == 0 && rp->cat[0]) { snprintf(icat, sizeof(icat), "%s", rp->cat); break; }
                    }
                }
                if (!icat[0] && meta.cat[0]) snprintf(icat, sizeof(icat), "%s", meta.cat);
                if (!icat[0]) snprintf(icat, sizeof(icat), "软件");
                /* 中文分类 → 安装目录: 游戏→engine / 运维·工具→ops / 其它→app */
                const char *pcat = "app";
                if (strcmp(icat, "游戏") == 0) pcat = "engine";
                else if (strcmp(icat, "运维") == 0 || strcmp(icat, "工具") == 0) pcat = "ops";
                snprintf(s_pkg[k].cat, sizeof(s_pkg[k].cat), "%s", pcat);
                snprintf(s_pkg[k].dst, sizeof(s_pkg[k].dst), "%s/%s/%s", AM_STORE_DST, pcat, nm);
                snprintf(s_pkg[k].dst_sd, sizeof(s_pkg[k].dst_sd), "/sdcard/apps/%s/%s", pcat, nm);
                /* ★ 2026-10-17 linit: **一律装到内置** (/store/apps), 不再默认装 TF 卡 */
                s_pkg[k].pref = 0;
                (void)am_pkg_prefer;
                s_pkg[k].app.id        = s_pkg[k].id;
                s_pkg[k].app.label     = s_pkg[k].label;
                s_pkg[k].app.icon_idx  = am_store_icon_of(nm, s_pkg[k].cat_zh);
                s_pkg[k].app.page      = OS_PAGE_LUA;
                s_pkg[k].app.hidden    = true;
                s_pkg[k].app.touch_only= false;
                /* ★★★ 2026-10-17 修: 必须拷进结构体自己的字段再指过去 ——
                 *   原来直接指局部数组 icat ⇒ 悬空指针 ⇒ 分类过滤读垃圾 ⇒ 全部掉默认"软件"。 */
                snprintf(s_pkg[k].cat_zh, sizeof(s_pkg[k].cat_zh), "%s",
                         (strcmp(icat, "工具") == 0) ? "运维" : icat);
                s_pkg[k].app.cat       = s_pkg[k].cat_zh;
                ESP_LOGI("AMSTORE", "扫描包: %-16s cat='%s' app.cat='%s' dir=%s",
                         nm, s_pkg[k].cat, s_pkg[k].app.cat, bases[b]);
                s_pkg[k].app.reveal    = false;
            }
            snprintf(s_pkg[k].src, sizeof(s_pkg[k].src), "%s/%s", bases[b], nm);
            /* ★★★ 2026-10-17 linit: 在**已安装目录**里扫到的条目, dst 必须是"它实际所在的那个文件" ——
             *   否则卸载会删另一条路径 ⇒ 文件残留、商店里一直在(linit 报"卸载了还在"的根因)。
             *   b>=2 = /store/apps/{app,engine,ops} 已安装目录。 */
            if (b >= 2) {
                snprintf(s_pkg[k].dst, sizeof(s_pkg[k].dst), "%s/%s", bases[b], nm);
                s_pkg[k].dst_sd[0] = 0;
                ESP_LOGI("AMSTORE", "已安装条目: %s (cat=%s)", s_pkg[k].src, s_pkg[k].cat);
            }
            /* 缓存安装状态: 仅扫描时算一次, 渲染不再逐帧 fopen */
            s_pkg_inst[k] = am_file_exist(s_pkg[k].dst) || am_file_exist(s_pkg[k].dst_sd);
        }
        closedir(d);
    }
    /* ⚠ 按文件名去重 (2026-10-02): am_store_invalidate() 曾经把 s_pkg_busy 清零,
     * 扫描还没跑完就允许再起一个任务 → 两个任务交错写同一个 s_pkg[] →
     * 商店里出现**同名重复条目**(实测日志里"状态已刷新"打印了 3 次)。
     * 这里做一次保序去重作为最后防线。
     * ⚠ 搬完必须重指 app.id/app.label —— 它们是"指向自己数组"的自指针, 整体拷贝后会指到旧槽位。 */
    {
        int w = 0;
        for (int r = 0; r < s_pkg_n; r++) {
            bool dup = false;
            for (int q = 0; q < w; q++)
                if (strcmp(s_pkg[q].fname, s_pkg[r].fname) == 0) { dup = true; break; }
            if (dup) continue;
            if (w != r) {
                s_pkg[w] = s_pkg[r];
                s_pkg[w].app.id    = s_pkg[w].id;
                s_pkg[w].app.label = s_pkg[w].label;
            }
            w++;
        }
        if (w != s_pkg_n) ESP_LOGD(TAG, "商店包去重: %d -> %d 条", s_pkg_n, w);
        s_pkg_n = w;
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
        ESP_LOGD(TAG, "商店扫描任务创建失败(内存不足?), 保留上次结果并允许重试");
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

/* ---- 云端视图 ---- */
static int am_rview_find(const os_app_t *a) {
    for (int i = 0; i < s_rview_n; i++)
        if (&s_rview[i].app == a) return i;
    return -1;
}
/* 按当前索引重建云端视图 (UI 线程调用; 索引没变就什么都不做)。
 * ⚠ 已下载到商店源目录的包要**剔掉** —— 否则它会以"云端条目"再出现一次, 同一个包
 *   在列表里显示两条(一条可安装、一条还在"下载"), 与"商店里两个 BIOS 模拟器"同款 bug。 */
static void am_store_sync_remote(void) {
    if (!am_store_ready()) return;
    int want = am_rstore_count();
    if (want == s_rview_synced) return;
    s_rview_n = 0;
    for (int i = 0; i < want && s_rview_n < AM_RVIEW_MAX; i++) {
        const am_rpkg_t *rp = am_rstore_get(i);
        if (!rp || !rp->name[0]) continue;
        char fn[48];
        snprintf(fn, sizeof(fn), "%s.lua", rp->name);
        bool dup = false;
        for (int k = 0; k < s_pkg_n; k++)
            if (strcmp(s_pkg[k].fname, fn) == 0) { dup = true; break; }
        if (dup) continue;                       /* 本地已有 → 走本地那条 */
        am_pkg_entry_t *e = &s_rview[s_rview_n];
        memset(e, 0, sizeof(*e));
        e->kind  = 1;
        e->ridx  = (int16_t)i;
        e->pref  = 0;
        snprintf(e->fname, sizeof(e->fname), "%s", fn);
        snprintf(e->label, sizeof(e->label), "%s", rp->label);
        /* ★★★ 2026-10-17 linit: 按索引里的 cat 归类(不再一律塞"软件")。
         *   索引 cat: game/ops/app ; 本地合法目录: engine/ops/app(无 game) ⇒ game→engine(独立游戏)。
         *   安装目录随分类变化, 下载前后同源(本地扫描按目录判分类 ⇒ 一致)。 */
        const char *icat = (rp->cat[0]) ? rp->cat : "程序";   /* 索引里的中文分类 */
        const char *cat  = (strcmp(icat, "游戏") == 0) ? "engine"
                         : (strcmp(icat, "运维") == 0 || strcmp(icat, "工具") == 0) ? "ops" : "app";
        snprintf(e->cat,   sizeof(e->cat),   "%s", cat);
        snprintf(e->id,    sizeof(e->id),    "pkg:%s", fn);
        snprintf(e->src,   sizeof(e->src),   "%s/%s", AM_STORE_SRC2, fn);
        snprintf(e->dst,   sizeof(e->dst),   "%s/%s/%s", AM_STORE_DST, cat, fn);
        snprintf(e->dst_sd,sizeof(e->dst_sd),"/sdcard/apps/%s/%s", cat, fn);
        e->app.id   = e->id;
        e->app.label = e->label;
        e->app.icon_idx = am_store_icon_of(fn, e->cat_zh);
        e->app.page   = OS_PAGE_LUA;
        e->app.hidden = true;
        e->app.touch_only = false;
        /* ★★★ 2026-10-17: 商店顶部子分类用的是**中文分类名**(与内置应用 .cat 同一套:
         *   游戏/软件(程序)/系统/运维/学习) —— 之前直接塞英文 "engine/ops/app" ⇒ 全部对不上
         *   ⇒ 所有云包都掉进"其他"。这里做一次映射: 安装目录仍用英文(e->cat), 显示用中文。 */
        /* ★ 同上: 拷进结构体字段, 避免指向局部数组 */
        snprintf(e->cat_zh, sizeof(e->cat_zh), "%s",
                 (strcmp(icat, "工具") == 0) ? "运维" : icat);
        e->app.cat = e->cat_zh;
        e->app.reveal = false;
        s_rview_n++;
    }
    s_rview_synced = want;
}
/* 总条目数 / 取第 k 条: 前 s_pkg_n 条是本地包, 后面接云端视图 */
static int am_total_n(void) { return (s_pkg_scanned == 0) ? (s_pkg_n + s_rview_n) : 0; }
static const am_pkg_entry_t *am_entry(int k) {
    if (k < 0 || k >= am_total_n()) return NULL;
    return (k < s_pkg_n) ? &s_pkg[k] : &s_rview[k - s_pkg_n];
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
    /* ★ 2026-10-17 诊断: 安装到底从哪复制到哪、cat 是什么 —— "报成功但应用里看不到" 时看这行 */
    ESP_LOGI("AMSTORE", "安装: k=%d cat='%s' src=%s dst=%s", k, s_pkg[k].cat, s_pkg[k].src, dest);
    if (am_copy_file(s_pkg[k].src, dest)) {
        ESP_LOGI("AMSTORE", "安装完成: %s", dest);
        os_dialog_toast(ctx, to_tf ? "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85\xe5\x88\xb0TF\xe5\x8d\xa1"   /* 已安装到TF卡 */
                                   : "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85\xe5\x88\xb0\xe5\x86\x85\xe7\xbd\xae"); /* 已安装到内置 */
        am_lua_invalidate();   /* Lua 列表下次重扫 */
        am_reload_pkg_inst(k);                       /* 安装后只刷新该包缓存一次 */
    } else {
        os_dialog_toast(ctx, "\xe5\xae\x89\xe8\xa3\x85\xe5\xa4\xb1\xe8\xb4\xa5");   /* 安装失败 */
    }
    am_rebuild_keep();
    /* ★★★ 2026-10-17 linit: 原来这里 os_dialog_pop —— 但 toast 已经把确认框关掉了,
     *   再 pop 一次就把**商店页本身**弹出去了("安装完就退出商店"的根因)。
     *   改用 mark_handled: 只关闭当前弹窗, 不额外弹栈。 */
    os_dialog_mark_handled(ctx);
    ctx->needs_redraw = true;
}
/* ========== 商店 云端包: 先下载到商店源目录, 再走普通安装 (2026-10-04) ==========
 * 下载成功 → 文件落在 /sdcard/store_pkg (或 SPIFFS /store 根) → 与用户自己拷进去的包
 * **完全等价**, 重扫后就从"云端条目"变成"本地条目", 安装逻辑一行都不用改。 */
static void am_store_dl_cb(ui_ctx_t *ctx, int result, void *ud) {
    int rk = (int)(long)ud;
    if (result != 0) { am_rebuild_keep(); return; }   /* 取消: 弹窗由框架自动关 */
    int rpos = (rk >= 0 && rk < s_rview_n) ? s_rview[rk].ridx : -1;
    if (rpos < 0 || !am_rstore_dl_start(rpos))
        os_dialog_toast(ctx, "\xe6\x97\xa0\xe6\xb3\x95\xe5\xbc\x80\xe5\xa7\x8b\xe4\xb8\x8b\xe8\xbd\xbd"); /* 无法开始下载 */
    am_rebuild_keep();
    os_dialog_mark_handled(ctx);   /* ★ 同上: 不再多 pop 一层 */
    ctx->needs_redraw = true;
}
/* 云端条目被点 → 弹"下载 XX (123KB)?" */
static void am_store_download_ask(ui_ctx_t *ctx, int rk) {
    if (rk < 0 || rk >= s_rview_n) return;
    const am_rpkg_t *rp = am_rstore_get(s_rview[rk].ridx);
    char title[72];
    if (rp)
        snprintf(title, sizeof(title), "\xe4\xb8\x8b\xe8\xbd\xbd %s?", rp->label);   /* 下载 XX? (不再带大小) */
    else
        snprintf(title, sizeof(title), "\xe4\xb8\x8b\xe8\xbd\xbd %s?", s_rview[rk].label);
    os_dialog_confirm_ex(ctx, title, 0, 0, am_store_dl_cb, (void *)(long)rk);
}

/* ★ 商店行统一入口 (2026-10-02 linit: "商店只负责安装/卸载, 卸载不负责启动应用"):
 *   已安装 → 弹"卸载 XXX?" ; 未安装 → 弹"安装 XXX?" ; 云端未下载 → 弹"下载 XXX?" 。
 *   **商店里永远不启动应用** —— 要运行请到「应用」分类点它。
 *   (原来已安装时走这里会直接运行, 用户点了按钮却跑起来、卸载还藏在长按里, 等于点不到) */
void am_store_toggle(ui_ctx_t *ctx, const os_app_t *a) {
    if (!a) return;
    int rk = am_rview_find(a);
    if (rk >= 0) { am_store_download_ask(ctx, rk); return; }   /* 云端包: 先下载 */
    int k = am_pkg_find(a);
    if (k < 0) return;
    if (am_pkg_installed(k)) { am_store_uninstall_ask(ctx, a); return; }
    am_store_activate(ctx, a);          /* 未装时 activate 的本职就是弹"安装?"确认 */
}

/* 商店选中: 已安装(内置或卡) → 运行; 未安装 → 弹"安装 XXX?" 确认 (目标是应用默认, 不选) */
void am_store_activate(ui_ctx_t *ctx, const os_app_t *a) {
    int rk = am_rview_find(a);
    if (rk >= 0) { am_store_download_ask(ctx, rk); return; }   /* 云端包: 先下载 */
    int k = am_pkg_find(a);
    if (k < 0) return;
    if (am_pkg_installed(k)) {
        const char *run = am_file_exist(s_pkg[k].dst) ? s_pkg[k].dst : s_pkg[k].dst_sd; /* 优先内置, 否则TF卡 */
        am_lua_open_path(ctx, run);     /* 统一入口: 带 `need=wifi` 门禁 (未联网先弹窗连网) */
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
    os_dialog_mark_handled(ctx);   /* ★ 2026-10-17: 不再多 pop 一层(否则把商店页弹走) */
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
int  am_store_count(void) { return am_total_n(); }

const os_app_t *am_store_app(int k) {
    const am_pkg_entry_t *e = am_entry(k);
    return e ? &e->app : NULL;
}
bool am_store_owns(const os_app_t *a) { return am_pkg_find(a) >= 0 || am_rview_find(a) >= 0; }
bool am_store_installed_of(const os_app_t *a) {
    int k = am_pkg_find(a);
    /* 读缓存: 渲染路径不能逐帧 fopen。云端包 (kind=1) 永远不算"已装"—— 还没下载呢 */
    return (k >= 0) && s_pkg_inst[k];
}
const char *am_store_fname_of(const os_app_t *a) {
    int k = am_pkg_find(a);
    if (k >= 0) return s_pkg[k].fname;
    int r = am_rview_find(a);
    return (r >= 0) ? s_rview[r].fname : NULL;
}
/* 商店行第二行文案: 本地包 → LAPP 简介 (可能 NULL → 不画);
 *                    云端包 → "未下载 · NKB" (让用户知道点了要下多少) */
const char *am_store_sub_of(const os_app_t *a) {
    int k = am_pkg_find(a);
    if (k >= 0) return s_pkg[k].desc[0] ? s_pkg[k].desc : NULL;
    int r = am_rview_find(a);
    if (r < 0) return NULL;
    const am_rpkg_t *rp = am_rstore_get(s_rview[r].ridx);
    if (!rp) return NULL;
    /* ★ 2026-10-17 linit: 第二行统一显示**应用说明**(不再显示"未下载 · NKB") */
    return rp->desc[0] ? rp->desc : NULL;
}
/* 右侧按钮文案: 下载 / 卸载 / 安装 (同一行只会是其中一种) */
const char *am_store_btn_of(const os_app_t *a) {
    if (am_rview_find(a) >= 0) return "\xe4\xb8\x8b\xe8\xbd\xbd";        /* 下载 */
    return am_store_installed_of(a) ? "\xe5\x8d\xb8\xe8\xbd\xbd"          /* 卸载 */
                                    : "\xe5\xae\x89\xe8\xa3\x85";          /* 安装 */
}
/* 目录变化(装入/卸载/下载完成)后调用: 下次进商店重新扫描。
 * ⚠ 不要清 s_pkg_busy —— 扫描任务可能还在跑, 清了就允许**再起一个并发扫描**,
 *   两个任务交错写 s_pkg[] 会产生同名重复条目(2026-10-02 linit 报的"商店里有两个"). */
void am_store_invalidate(void) {
    s_pkg_scanned = -1;
    s_pkg_done = false;
    s_rview_synced = -1;      /* 云端视图也要重算(刚下载的包要从"云端"挪到"本地") */
}

/* 每帧取货: ① 后台扫描完成 → 刷新安装状态缓存 ② 按索引重建云端视图;
 * 返回 true = UI 应重建列表 */
bool am_store_tick(void) {
    bool changed = false;
    if (s_pkg_done) {
        s_pkg_done = false;
        am_pkg_cleanup();
        changed = true;
    }
    int before = s_rview_n;
    am_store_sync_remote();
    if (s_rview_n != before) changed = true;
    return changed;
}
