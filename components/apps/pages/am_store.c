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
#include "am_asset.h"   /* ★ 2026-10-25 资源包卸载(递归删目录) */
#include "font_book.h"  /* 2026-10-30: 查询"正在使用的字体"以免卸掉它 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "am_internal.h"
#include "apps.h"   /* apps_builtin_has_id: 商店扫描去重 */
#include "wifi_manager.h"   /* ★ 下载前判断是否联网 */
#include "esp_timer.h"   /* ★ 扫描 busy 超时判断 */
#include "apppack.h"   /* LAPP 单文件资源包: 取名称/简介 */
#include "net_client.h"
#include "sd_scan.h"
#include "esp_spiffs.h"   /* 2026-10-31: 移动应用到内置前查 apps 分区余量 */   /* sd_is_mounted: 默认装 TF 卡的前提判断 */   /* ★ 2026-10-30 下载目录掉电自愈: netc_busy / netc_recover_path */

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
    /* ★ 2026-10-27 商店"更新"能力: 本地已装版本 vs 云端索引版本。
     *   ver  = 从已安装文件的 LAPP 头 `ver=` 读出来的实际版本
     *   rver = 云端 index.json 里同名包的 ver (0 = 索引里没有/字段缺失 = 不提示更新)
     *   判定: 已安装 且 rver > ver  ⇒ 按钮显示「更新」 */
    uint16_t ver;
    uint16_t rver;
    /* ★ 2026-10-30 linit 发版规则: 更新判定**以构建时间为准**。
     *   lbuild = 已安装文件 LAPP 头里的 build=YYYYMMDDHHMM
     *   rbuild = 云端 index.json 里同名包的 build
     *   两者都非空 → 用 strcmp 比时间(定长数字, 字典序==时间序);
     *   任一为空(老包/老索引) → 退回 rver > ver 的旧规则。
     *   这样"忘了改版本号"不会再导致推不出更新, 版本号只用来给人看。 */
    char     lbuild[16];
    char     rbuild[16];
    uint8_t  kind;       /* 0=本地已有包(可安装)  1=云端包(需先下载) —— 2026-10-04 */
    int16_t  ridx;       /* kind=1 时: 对应 am_rstore_get() 的下标 */
} am_pkg_entry_t;
static EXT_RAM_BSS_ATTR am_pkg_entry_t s_pkg[AM_PKG_MAX];
static int s_pkg_n = 0;
static volatile int s_pkg_scanned = -1;   /* 0=已扫描 (数据可用)  -1=未扫/扫描中 (后台任务写入) */
static volatile bool s_pkg_busy  = false;   /* 后台扫描任务进行中 */
static volatile bool s_pkg_done  = false;   /* 后台扫描已完成 (UI 线程取货) */
static volatile bool s_uninst_done = false;   /* ★ 后台卸载刚完成 ⇒ UI 需刷新一次 */
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
/* 安装目标: 0=内置固件 /store/apps, 1=TF 卡 /sdcard/apps。
 * ★★★ 2026-10-24 linit 定案: **所有应用默认装 TF 卡**（/sdcard/apps/<分类>/）。
 *   固件内置 SPIFFS(apps 分区) 不再放任何应用 —— 出厂只有 _ui.lua。
 *   好处: ① 4MB apps 分区彻底让给"引擎包"等将来用途 ② 应用随卡走, 换机不用重下
 *   ③ 删应用不动 flash。要改回内置: 返回 0。 */
static int am_pkg_prefer(const char *fname) { (void)fname; return 1; }
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
    /* ★ 2026-10-25 linit: 动态壁纸/资源包 用自己的图标, 不再回退成"应用中心" */
    if (strcmp(cat, "壁纸") == 0 ||
        strcmp(cat, "动态壁纸") == 0 ||
        strcmp(cat, "资源") == 0 ||
        strcmp(cat, "wallpaper") == 0) return ICON_WALLPAPER;
    if (strcmp(cat, "engine") == 0) return ICON_MINI_GAMES;   /* 兼容旧目录名 */
    if (strcmp(cat, "ops") == 0)    return ICON_TERMINAL;
    if (strcmp(cat, "game") == 0)   return ICON_MINI_GAMES;
    if (strcmp(cat, "study") == 0)  return ICON_BOOK;
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

    /* ★★★ 2026-10-30 提速: 原来是 512B **栈**缓冲。
     *   改成 32KB 之后, 29MB 的资源包从约 5.7 万次 fread/fwrite 降到约 900 次。
     *   ⚠ 说清楚收益边界(别把它当"卡速翻倍"): FATFS 的扇区是 4KB
     *     (CONFIG_FATFS_SECTOR_4096), 512B 顺序写**不会**触发读-改-写 ——
     *     前 7 次只在扇区缓存里累加, 第 8 次才真正落卡。
     *     所以这里省的是**每次 f_write 的锁/查表/memcpy 开销**, 不是 SD 总线时间;
     *     真实收益在小块多、文件多的场景, 纯粹大文件顺序复制的体感提升有限。
     *   ⚠ 不用栈缓冲: 本函数所在任务栈可能只有几 KB, 4KB 数组会把栈顶掉
     *     (踩过: remcon 把 usStackDepth 当 word 用 → 4KB 栈 → 整机重启)。
     *   ⚠ 静态缓冲: am_copy_file 只在商店安装的后台任务里串行调用, 不存在并发。 */
    #define AM_CP_BUF_SZ (32 * 1024)
    static char *s_cp_buf = NULL;
    if (!s_cp_buf) {
        s_cp_buf = heap_caps_malloc(AM_CP_BUF_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cp_buf) s_cp_buf = heap_caps_malloc(AM_CP_BUF_SZ, MALLOC_CAP_DEFAULT);
        if (!s_cp_buf) { fclose(in); fclose(out); return false; }   /* 分配失败 → 如实失败 */
    }

    bool ok = true;
    size_t n;
    while ((n = fread(s_cp_buf, 1, AM_CP_BUF_SZ, in)) > 0)
        if (fwrite(s_cp_buf, 1, n, out) != n) { ok = false; break; }
    if (ferror(in)) ok = false;          /* 读出错不能被当成"复制完了" */
    if (fclose(out) != 0) ok = false;    /* 写盘收尾失败(f_write 缓冲未落盘)必须报出来 */
    fclose(in);
    #undef AM_CP_BUF_SZ
    return ok;
}
static int am_pkg_find_name(const char *fname) {
    for (int i = 0; i < s_pkg_n; i++)
        if (strcmp(s_pkg[i].fname, fname) == 0) return i;
    return -1;
}
/* ============ 下载目录掉电自愈 (2026-10-30) ============
 * 背景: 下载走 "xxx.lua.part → 替换 → xxx.lua", 替换时**旧文件先改名成 xxx.lua.bak**。
 *   若在"旧文件已成 .bak、新文件还没就位"之间掉电, 正式文件名就是缺失的; 而恢复原先
 *   只在**再次下载同一路径**时触发 ⇒ 用户不重下, 那个包就一直"消失"。
 *   这里在商店扫描开头补一次轻量恢复, 复用 net_client 的**同一套**逻辑
 *   (netc_recover_path → netc_fs_recover), 不复制第二套状态机。
 *
 * 规则:
 *   - 只扫 /sdcard/store_pkg(本下载目录), **不碰**已安装目录 /store/apps、/sdcard/apps;
 *   - 只认 `*.lua.bak`: 正式文件缺失 → 复位; 正式文件在 → 删陈旧 .bak(正式文件优先);
 *   - **.part 一律不删也不当包**: 它可能正被另一个任务下载, 删了会毁掉别人的传输;
 *     扫描的 ".lua 结尾" 过滤天然也不会把它当包(它结尾是 .part);
 *   - 下载进行中(netc_busy)直接跳过 —— 否则会和正在做的 rename 抢同一个文件;
 *   - 没有 .bak 时每个候选只花一次 stat、不做任何写操作 ⇒ 每次刷新都跑也几乎零成本。
 */
static void am_store_recover_baks(void)
{
    if (netc_busy()) return;                    /* 下载中: 交给下载侧自己收尾 */
    DIR *d = opendir(AM_STORE_SRC2);
    if (!d) return;                             /* TF 卡没挂载 */
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t len = strlen(nm);
        if (len <= 8 || strcasecmp(nm + len - 8, ".lua.bak") != 0) continue;
        if (strchr(nm, '/')) continue;
        char fin[192];                          /* 只建正式名; .bak 名由 net_client 内部拼 */
        snprintf(fin, sizeof(fin), "%s/%.*s", AM_STORE_SRC2, (int)len - 4, nm);
        if (netc_recover_path(fin) != 0) n++;   /* RESTORED / CLEANED / FAILED 都算处理过 */
    }
    closedir(d);
    if (n) ESP_LOGW(TAG, "下载目录自愈: 处理 %d 个 .bak", n);
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
    /* ★★★ 2026-10-27 linit「哪怕在内存卡里面也显示安装」的根因修复:
     *   原来只扫 **内置** 已安装目录 /store/apps/{app,engine,ops}, **完全没扫 TF 卡**
     *   ⇒ 装在 /sdcard/apps 下的应用在商店里根本不存在这条记录, 只能从云端索引露一行,
     *     而云端行的按钮天然是"下载/安装" ⇒ 明明装了却显示"安装"。
     *   现在把 TF 已安装目录也扫进来, 并且把 dst/dst_sd **两条候选路径都填上** ——
     *   卸载时才能把"内置一份 + TF 一份"一起删干净(linit 定: 卸载必须删干净)。
     *   b < 2  = 待下载源(/store 根, /sdcard/store_pkg)
     *   b 2..4 = 内置已安装(/store/apps/{app,engine,ops})
     *   b 5..7 = TF 已安装(/sdcard/apps/{app,engine,ops}) */
    /* ★ 2026-10-30: 先把下载目录里掉电残留的 .bak 复位, **再**列目录 ——
     *   顺序不能反, 否则本次扫描仍然看不到刚恢复出来的包(要等下一次刷新)。 */
    am_store_recover_baks();
    const char *bases[8] = { AM_STORE_SRC1, AM_STORE_SRC2,
                             AM_STORE_DST "/app", AM_STORE_DST "/engine", AM_STORE_DST "/ops",
                             "/sdcard/apps/app", "/sdcard/apps/engine", "/sdcard/apps/ops" };
    for (int b = 0; b < 8; b++) {
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
            /* ★★★ 2026-10-30 P0-1 修「中英文重复应用」:
             *   已安装目录 (b >= 2) 里的 xxx.lua, 若其**文件名主干**正好是固件内置应用的 id,
             *   说明这个应用已经在主菜单/应用中心有一条中文记录了 —— 商店里再生成一条
             *   id="pkg:xxx.lua"(英文文件名) 就是那条重复的英文条目。
             *   ⇒ 直接跳过, 不生成商店条目。
             *   ⚠ 只跳过"已安装目录(b>=2)"; 待下载源(b<2)不跳, 否则云端没装过的包也没了。
             *   ⚠ 不删文件、不删注册表、不影响启动和卸载。 */
            if (b >= 2 && len > 4) {
                char stem[64];
                snprintf(stem, sizeof(stem), "%.*s", (int)len - 4, nm);
                if (apps_builtin_has_id(stem)) continue;
            }
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
                /* ★ 2026-10-27: 读本地已装版本 (LAPP 头 `ver=`) —— 这是"更新"判定的左边。
                 *   注意 apppack.c 原来只认 "version=", 与文档/store_build 的 "ver=" 对不上,
                 *   已在 apppack.c 里两个键都收; 这里按整数解析 (非数字 → 0 = 未知)。 */
                s_pkg[k].ver = (uint16_t)strtoul(meta.version, NULL, 10);
                /* ★ 2026-10-30: 本地构建时间 —— "更新"判定的主依据(优先于版本号) */
                snprintf(s_pkg[k].lbuild, sizeof(s_pkg[k].lbuild), "%s", meta.build);
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
                        if (rp && strcmp(rp->name, bn) == 0) {
                            /* ★ 2026-10-27: 顺手记下云端版本 —— "更新"按钮靠它比大小 */
                            s_pkg[k].rver = (uint16_t)rp->ver;
                            /* ★ 2026-10-30: 云端构建时间 (更新判定的主依据) */
                            snprintf(s_pkg[k].rbuild, sizeof(s_pkg[k].rbuild),
                                     "%s", rp->build);
                            if (rp->cat[0]) { snprintf(icat, sizeof(icat), "%s", rp->cat); break; }
                        }
                    }
                }
                if (!icat[0] && meta.cat[0]) snprintf(icat, sizeof(icat), "%s", meta.cat);
                if (!icat[0]) snprintf(icat, sizeof(icat), "软件");
                /* 中文分类 → 安装目录: 游戏→engine / 运维·工具→ops / 其它→app */
                const char *pcat = "app";
                /* ★ 2026-10-18 linit: **游戏改装到 app/(应用→游戏)**, 引擎只留模拟器 */
                if (strcmp(icat, "游戏") == 0) pcat = "app";
                else if (strcmp(icat, "运维") == 0 || strcmp(icat, "工具") == 0) pcat = "ops";
                snprintf(s_pkg[k].cat, sizeof(s_pkg[k].cat), "%s", pcat);
                snprintf(s_pkg[k].dst, sizeof(s_pkg[k].dst), "%s/%s/%s", AM_STORE_DST, pcat, nm);
                snprintf(s_pkg[k].dst_sd, sizeof(s_pkg[k].dst_sd), "/sdcard/apps/%s/%s", pcat, nm);
                /* ★ 2026-10-17 linit: **一律装到内置** (/store/apps), 不再默认装 TF 卡 */
                s_pkg[k].pref = 0;
                (void)am_pkg_prefer;
                s_pkg[k].app.id        = s_pkg[k].id;
                s_pkg[k].app.label     = s_pkg[k].label;
                s_pkg[k].app.page      = OS_PAGE_LUA;
                s_pkg[k].app.hidden    = true;
                s_pkg[k].app.touch_only= false;
                /* ★★★ 2026-10-17 修: 必须拷进结构体自己的字段再指过去 ——
                 *   原来直接指局部数组 icat ⇒ 悬空指针 ⇒ 分类过滤读垃圾 ⇒ 全部掉默认"软件"。 */
                snprintf(s_pkg[k].cat_zh, sizeof(s_pkg[k].cat_zh), "%s",
                         (strcmp(icat, "工具") == 0) ? "运维" : icat);
                s_pkg[k].app.cat       = s_pkg[k].cat_zh;
                /* ★★★ 2026-10-25 修: icon 必须在 cat_zh 填好**之后**再算 ——
                 *   原来在上面先算, cat_zh 还是空串, am_store_cat_icon("") 一律返回
                 *   ICON_APP_MGR ⇒ 商店所有包都长同一个"应用中心"图标。 */
                s_pkg[k].app.icon_idx  = am_store_icon_of(nm, s_pkg[k].cat_zh);
                ESP_LOGI("AMSTORE", "扫描包: %-16s cat='%s' app.cat='%s' dir=%s",
                         nm, s_pkg[k].cat, s_pkg[k].app.cat, bases[b]);
                s_pkg[k].app.reveal    = false;
            }
            snprintf(s_pkg[k].src, sizeof(s_pkg[k].src), "%s/%s", bases[b], nm);
            /* ★★★ 2026-10-17 linit: 在**已安装目录**里扫到的条目, dst 必须是"它实际所在的那个文件" ——
             *   否则卸载会删另一条路径 ⇒ 文件残留、商店里一直在(linit 报"卸载了还在"的根因)。
             *   b>=2 = /store/apps/{app,engine,ops} 已安装目录。 */
            if (b >= 2) {
                /* ★ 2026-10-27: 已安装目录里扫到的条目 —— **两条候选路径都填**:
                 *   内置目录 → dst=实际文件, dst_sd=TF 同名候选
                 *   TF 目录   → dst_sd=实际文件, dst=内置同名候选
                 *   (原来 b>=2 一律把 dst_sd 清空 ⇒ 只有 TF 装了也判"未安装",
                 *    而且有内置+TF 两份时卸载只会删掉一份) */
                const char *dc = strrchr(bases[b], '/'); dc = dc ? dc + 1 : bases[b];
                if (b <= 4) {
                    snprintf(s_pkg[k].dst,    sizeof(s_pkg[k].dst),    "%s/%s", bases[b], nm);
                    snprintf(s_pkg[k].dst_sd, sizeof(s_pkg[k].dst_sd), "/sdcard/apps/%s/%s", dc, nm);
                    ESP_LOGI("AMSTORE", "已安装条目(内置): %s (dir=%s)", s_pkg[k].dst, dc);
                } else {
                    snprintf(s_pkg[k].dst_sd, sizeof(s_pkg[k].dst_sd), "%s/%s", bases[b], nm);
                    snprintf(s_pkg[k].dst,    sizeof(s_pkg[k].dst),    "%s/%s/%s", AM_STORE_DST, dc, nm);
                    ESP_LOGI("AMSTORE", "已安装条目(TF卡): %s (dir=%s)", s_pkg[k].dst_sd, dc);
                }
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
    /* 2026-10-31: 补一条可核对的日志 —— 之前"扫描到底有没有跑完"完全不可观测,
     *   排查"商店点不动/条目少了"时只能猜。 */
    ESP_LOGI(TAG, "商店本地扫描完成: %d 条 (内部 %s + TF卡 %s)", s_pkg_n,
             AM_STORE_DST, "/sdcard/apps");
}
/* 后台扫描任务: 商店包扫描 (SPIFFS/SD 慢, 不阻塞 UI 首帧) */
static void am_pkg_scan_task(void *arg) {
    (void)arg;
    am_pkg_scan();
    s_pkg_done = true;   /* 通知 UI 线程取货 (重建列表) */
    s_pkg_busy = false;
    vTaskDelete(NULL);
}
/* ★★★ 2026-10-18 linit: **每次联网进商店都后台自动更新索引** ——
 *   原来 `am_rstore_prefetch()` 只用 s_fetched 保证"只拉一次" ✗ ⇒ 永远看第一次的列表
 *   ⇒ 现在每次进商店(距上次刷新 > 60 秒)就把 s_fetched 清掉, 让它重新拉一次 ✓
 *   拉取在后台任务里做, 失败**不清旧数据**(离线仍能浏览) ✓ 不阻塞界面 ✓ */
void am_store_refresh_index_if_due(void) {
    extern bool am_rstore_refresh_due(void);
    if (am_rstore_refresh_due()) am_rstore_refresh_force();
}

void am_store_prefetch(void) {
    /* ★★★ 2026-10-18 linit **必修**: `s_pkg_busy` 一旦卡死(扫描任务异常退出/创建失败)
     *   就再也不会重扫 ⇒ `am_total_n()` 永远 0 ⇒ **商店空白 + 索引也不拉** ✗
     *   ⇒ 加 3 秒超时: 超时认为扫描已死, 强制解锁重来 ✓ */
    if (s_pkg_busy) {
        static int64_t busy_t0 = 0;
        if (busy_t0 == 0) busy_t0 = esp_timer_get_time();
        if ((esp_timer_get_time() - busy_t0) > 3000000) {
            ESP_LOGW(TAG, "商店扫描卡住超 3s ⇒ 强制解锁重扫");
            s_pkg_busy = false;
            s_pkg_scanned = -1;
            s_pkg_done = false;
            busy_t0 = 0;
        } else {
            return;
        }
    } else {
        ; /* 未 busy: 走下面的正常流程 */
    }
    /* ★★★ 2026-10-30 根因修复: 「商店里看不到新上架的资源」★★★
     *   原来这两行的顺序是反的：
     *       if (s_pkg_scanned == 0) return;      ← 本地包扫过一遍就永远返回
     *       ...索引刷新...
     *   ⇒ **本地扫描完成后，云端索引再也不会刷新**。
     *   服务器后来上架的字体/应用，用户要重启都看不到 —— 就是本次的现场。
     *
     *   现在把索引刷新提到 return 之前，并且理由写清楚：
     *   「本地包扫过一次」和「云端索引是最新的」是两件事，不能互相短路。
     *   刷新本身有 60 秒节流(am_rstore_refresh_due) + 后台任务，不会拖慢进店。
     */
    {
        extern void am_rstore_refresh_force(void);
        extern bool am_rstore_refresh_due(void);
        if (am_rstore_refresh_due()) am_rstore_refresh_force();
    }
    if (s_pkg_scanned == 0) return;
    s_pkg_busy = true;
    s_pkg_done = false;
    /* V1.2 修复: 原实现忽略返回值 —— 任务创建失败(内存不足)时 s_pkg_busy 永久为 true,
     * 商店会一直显示"正在扫描…"且无法重试 (prefetch 被 busy 挡退). 失败须回滚标志. */
    /* ★★★ 2026-10-18 linit **必修**: 栈 8192 → **4096**。
     *   实测开机内部 RAM 只剩 ~10KB, 8KB 栈的任务**创建失败** ✗
     *   ⇒ 扫描永不执行 ⇒ `s_pkg_scanned` 永远 != 0 ⇒ 商店空白 "无法获取应用列表" ✓✓
     *   (本任务只做 opendir/fopen/strcmp, 4KB 足够; 必须内部 RAM —— 读 flash 时禁缓存,
     *    PSRAM 栈会崩) */
    /* ★★★ 2026-10-30 修「点应用中心立刻死机重启」的真根因:
     *   串口抓到 "A stack overflow in task pkgscan has been detected."
     *   栈只有 4096 字节, 而扫描任务里有多层局部数组
     *   (spath[192] + nb[64] + stem[64] + LAPP 头解析缓冲) 叠加 fopen/snprintf/日志,
     *   再加开 DEBUG 后日志调用占栈 → 直接爆栈 panic。
     *   加到 8192 (与下载任务同级), 与"开不开日志"无关都能扛住。 */
    if (xTaskCreatePinnedToCore(am_pkg_scan_task, "pkgscan", 8192, NULL, 3, NULL, 0) != pdPASS) {
        s_pkg_busy = false;
        s_pkg_done = false;
        ESP_LOGW(TAG, "商店扫描任务创建失败(内存不足?) ⇒ 保留上次结果, 下帧重试");
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
    /* ★ 同上: 不再要求"包扫描完成" —— 没 TF 卡也要能显示云端列表 ✓ */
    if (am_rstore_count() == 0) return;
    int want = am_rstore_count();
    if (want == s_rview_synced) return;
    s_rview_n = 0;
    for (int i = 0; i < want && s_rview_n < AM_RVIEW_MAX; i++) {
        const am_rpkg_t *rp = am_rstore_get(i);
        if (!rp || !rp->name[0]) continue;
        char fn[48];
        /* ★★★ 2026-10-30: 资源包(asset, 如字体 .zip) **不能用 "<名字>.lua" 当文件名**。
         *   原来一律拼 .lua ⇒ 字体条目在列表里叫 "文泉驿点阵宋16.lua"：
         *   · 和本地 .lua 包去重时永远不相等（字体本来也不该和 .lua 比）；
         *   · "已安装"判定按 dst 找 .lua，永远 false；
         *   · 真去下载时若哪天回落到这条路径，会拼出 404 的 <名字>.lua。
         *   资源包一律用索引里 path 的真实文件名（font_wqy16.zip），与下载器一致。 */
        const bool is_asset = (strcmp(rp->type, "asset") == 0);
        if (is_asset) {
            const char *b = strrchr(rp->path, '/');
            snprintf(fn, sizeof(fn), "%s", b ? b + 1 : rp->path);
        } else {
            snprintf(fn, sizeof(fn), "%s.lua", rp->name);
        }
        bool dup = false;
        if (!is_asset) {
            for (int k = 0; k < s_pkg_n; k++)
                if (strcmp(s_pkg[k].fname, fn) == 0) { dup = true; break; }
            if (dup) continue;                   /* 本地已有同名的 .lua → 走本地那条 */
        }
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
        /* ★ 2026-10-18 linit: 游戏 → app(应用→游戏); 引擎分类只留模拟器 */
        const char *cat  = (strcmp(icat, "运维") == 0 || strcmp(icat, "工具") == 0) ? "ops" : "app";
        snprintf(e->cat,   sizeof(e->cat),   "%s", cat);
        snprintf(e->id,    sizeof(e->id),    "pkg:%s", fn);
        if (is_asset) {
            /* 资源包不装进 /store/apps/<cat>/，解到索引里声明的 target
             * （字体 → /sdcard/fonts）。dst 只用于"已安装"探测，指向 target 本身。 */
            snprintf(e->src, sizeof(e->src), "/sdcard/%s", fn);
            snprintf(e->dst, sizeof(e->dst), "%s", rp->target[0] ? rp->target : "/sdcard");
            e->dst_sd[0] = 0;
        } else {
            snprintf(e->src,   sizeof(e->src),   "%s/%s", AM_STORE_SRC2, fn);
            snprintf(e->dst,   sizeof(e->dst),   "%s/%s/%s", AM_STORE_DST, cat, fn);
            snprintf(e->dst_sd,sizeof(e->dst_sd),"/sdcard/apps/%s/%s", cat, fn);
        }
        e->app.id   = e->id;
        e->app.label = e->label;
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
        /* ★★★ 2026-10-25 同上: 必须在 cat_zh 赋值之后再算分类图标 */
        e->app.icon_idx = am_store_icon_of(fn, e->cat_zh);
        e->app.reveal = false;
        s_rview_n++;
    }
    s_rview_synced = want;
}
/* ★ 2026-10-27 商店「更新」判定的数据源:
 *   索引(am_rstore)是异步到达的 —— 扫描包时它可能还没到(rver=0), 之后到了必须补一次。
 *   渲染路径(am_store_btn_of)每帧都调, 不能每次都遍历索引, 所以用"代次"缓存:
 *   只在 ①扫描完成(s_pkg 变了) ②索引条数变化 时重算一次。 */
static int s_rver_gen = -1;
static void am_sync_rver(void) {
    int gen = am_rstore_count();
    for (int k = 0; k < s_pkg_n; k++) {
        char bn[48];
        snprintf(bn, sizeof(bn), "%s", s_pkg[k].fname);
        char *dot = strrchr(bn, '.');
        if (dot) *dot = 0;
        uint16_t rv = 0;
        char rb[16] = "";
        for (int r = 0; r < gen; r++) {
            const am_rpkg_t *rp = am_rstore_get(r);
            if (rp && rp->name[0] && bn[0] && strcmp(rp->name, bn) == 0) {
                rv = (uint16_t)rp->ver;
                snprintf(rb, sizeof(rb), "%s", rp->build);
                break;
            }
        }
        s_pkg[k].rver = rv;
        snprintf(s_pkg[k].rbuild, sizeof(s_pkg[k].rbuild), "%s", rb);
    }
    s_rver_gen = gen;
}
/* 该条目是否有新版可装。
 * ★ 2026-10-30 linit 发版规则变更: **以构建时间为准**, 版本号只用于展示。
 *   判定顺序:
 *   ① 云端有 build + 本地有 build → strcmp(rbuild, lbuild) > 0
 *      build 是 12 位定长数字(YYYYMMDDHHMM), 字典序 == 时间先后, 不用转数值。
 *   ② 云端有 build 但**本地没有**(2026-10-30 之前发布的包 LAPP 头里没这个字段)
 *      → 视为"老体系", 提示一次更新, 让用户升级到带 build 的新包。
 *        ⚠ 这是刻意的迁移行为: 统一版本号后 ver 会变(如 矢量地图 4→1),
 *          若退回 ver 比较, 老用户就永远收不到更新了。
 *   ③ 云端没有 build(老索引) → 退回旧规则 rver > ver, 保证完全向后兼容。 */
static bool am_pkg_has_update(int k) {
    if (k < 0 || k >= s_pkg_n) return false;
    if (!s_pkg_inst[k]) return false;
    if (s_pkg[k].rbuild[0]) {
        if (!s_pkg[k].lbuild[0]) return true;      /* 老包 → 迁移一次 */
        return (strcmp(s_pkg[k].rbuild, s_pkg[k].lbuild) > 0);
    }
    uint16_t lv = s_pkg[k].ver ? s_pkg[k].ver : 1;
    return (s_pkg[k].rver > lv);
}

/* 总条目数 / 取第 k 条: 前 s_pkg_n 条是本地包, 后面接云端视图 */
/* ★★★ 2026-10-18 linit **必修**: 原来 `(s_pkg_scanned == 0) ? ... : 0`
 *   ⇒ **没插 TF 卡时包扫描永远完不成** ⇒ 列表恒为 0 ⇒ "无法获取应用列表" ✗
 *   ⇒ 现在直接返回本地+云端条数; 扫描没完只是本地那部分为空, **云端列表照常显示** ✓ */
static int am_total_n(void) { return s_pkg_n + s_rview_n; }
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
/* ★★★ 2026-10-18 linit: **下载完成后自动安装**(一步到位, 不用再点"安装")。
 *   按文件名在本地包里找 → 复制到 /store/apps/{cat}/, 与手动"安装"完全等价。 */
bool am_store_auto_install(const char *fname) {
    if (!fname || !fname[0]) return false;
    /* 去掉 .lua 得到包名 */
    char stem[48];
    snprintf(stem, sizeof(stem), "%s", fname);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    /* ★ 2026-10-18: **不依赖 s_pkg**(扫描可能还没跑完) —— 直接按索引决定分类目录,
     *   从下载目录复制到 /store/apps/{cat}/ ✓ */
    const char *sub = "app";
    int n = am_rstore_count();
    for (int i = 0; i < n; i++) {
        const am_rpkg_t *rp = am_rstore_get(i);
        if (rp && strcmp(rp->name, stem) == 0) {
            if (strcmp(rp->cat, "游戏") == 0) sub = "app";   /* 游戏 → 应用→游戏 */
            else if (strcmp(rp->cat, "运维") == 0 || strcmp(rp->cat, "工具") == 0) sub = "ops";
            break;
        }
    }
    char src[192], dst[192], catdir[96];
    snprintf(src, sizeof(src), "/sdcard/store_pkg/%s", fname);
    if (!am_file_exist(src)) snprintf(src, sizeof(src), "%s/%s", AM_STORE_SRC1, fname);

    /* ★★★ 2026-10-27 「更新」支持（原来"推了新版本也升不上去"的根因就在这段）：
     *   ① 目标原来写死成内置 AM_STORE_DST，而商店安装按 pref 可能装在 TF 卡
     *      ⇒ 更新会在内置再放一份，卡上那份旧版照样被启动。现在目标跟着"已装在哪"走。
     *   ② 原来 `if (已存在) return true` —— **永不覆盖**。现在按大小判定：
     *      大小相同 = 同一版本，跳过写卡；不同 = 覆盖。
     *   ③ 覆盖走 .new + 旧版改 .old 再改名，任何一步失败都回滚到旧版，不会把应用搞丢。 */
    char path_int[192], path_sd[192];
    snprintf(path_int, sizeof(path_int), "%s/%s/%s", AM_STORE_DST, sub, fname);
    snprintf(path_sd,  sizeof(path_sd),  "/sdcard/apps/%s/%s", sub, fname);
    /* ★★★ 2026-10-31 linit: **默认一律装 TF 卡**，内置 /store/apps 只做兜底。
     *   内置 apps 分区只有 4MB（SPIFFS 固定开销后可用 ≈3.65MB），
     *   而 TF 卡动辄几十 G —— 新装的应用没有理由去挤内置。
     *   三种情况要分清（原来的 `on_sd = 卡上存在` 只覆盖了第二、三种）：
     *     ① 两边都没有（新装）+ 卡已挂载  → 装卡        ← 本轮新增
     *     ② 卡上已有（之前装过/更新）      → 更新卡
     *     ③ 内置已有（老数据/用户移进去的）→ 更新内置，**不要**再往卡上复制一份
     *   没插卡时才退回内置，保证无卡也能用。 */
    bool on_sd = am_file_exist(path_sd);
    if (!on_sd && !am_file_exist(path_int) && sd_is_mounted()) on_sd = true;
    if (on_sd) snprintf(catdir, sizeof(catdir), "/sdcard/apps/%s", sub);
    else       snprintf(catdir, sizeof(catdir), "%s/%s", AM_STORE_DST, sub);
    snprintf(dst, sizeof(dst), "%s/%s", catdir, fname);
    mkdir(AM_STORE_DST, 0755);
    mkdir(catdir, 0755);

    struct stat ss, ds;
    long src_sz = (stat(src, &ss) == 0) ? (long)ss.st_size : -1;
    if (src_sz <= 0) { ESP_LOGW("AMSTORE", "自动安装: 源文件读不到 %s", src); return false; }

    bool had_old = (stat(dst, &ds) == 0);
    if (had_old) {
        if ((long)ds.st_size == src_sz) {
            ESP_LOGI("AMSTORE", "自动安装: 已是同一版本(%ld B), 跳过 %s", src_sz, dst);
            return true;
        }
        ESP_LOGI("AMSTORE", "自动安装: 覆盖更新 %s (%ld -> %ld B)",
                 dst, (long)ds.st_size, src_sz);
    }

    char tmp[200], bak[200];
    snprintf(tmp, sizeof(tmp), "%s.new", dst);
    snprintf(bak, sizeof(bak), "%s.old", dst);
    remove(tmp); remove(bak);
    if (!am_copy_file(src, tmp)) {
        remove(tmp);
        ESP_LOGW("AMSTORE", "自动安装失败(复制): %s", src);
        return false;
    }
    struct stat ts;
    if (stat(tmp, &ts) != 0 || (long)ts.st_size != src_sz) {
        remove(tmp);
        ESP_LOGW("AMSTORE", "自动安装失败(校验不通过, 可能是空间不足): %s", tmp);
        return false;
    }
    if (had_old && rename(dst, bak) != 0) {       /* 先把旧版挪开(失败也不丢) */
        remove(tmp);
        ESP_LOGW("AMSTORE", "自动安装失败(旧版挪不动): %s", dst);
        return false;
    }
    if (rename(tmp, dst) != 0) {
        remove(tmp);
        if (had_old) rename(bak, dst);            /* ← 回滚到旧版 */
        ESP_LOGW("AMSTORE", "自动安装失败(改名), 已回滚旧版: %s", dst);
        return false;
    }
    if (had_old) remove(bak);

    /* 另一侧还留着旧副本 → 删掉，否则同一个应用两份，启动的是旧的那份 */
    const char *other = on_sd ? path_int : path_sd;
    if (am_file_exist(other)) {
        remove(other);
        ESP_LOGW("AMSTORE", "自动安装: 已清理另一份旧副本 %s", other);
    }

    ESP_LOGI("AMSTORE", "自动安装完成: %s -> %s", src, dst);
    am_lua_invalidate();
    am_store_invalidate();
    return true;
}

/* 安装目标选择回调: result 0=内置固件 1=TF卡 */
/* 确认安装回调 (V1.6.x): 安装目标不再让用户选择, 由应用自身默认 s_pkg[k].pref 决定
 * (结果 0=确认安装; 1=取消). 现有应用默认装内置; 以后大的游戏在对应应用设默认装 TF. */
/* ★ 2026-10-18 linit: 商店的 安装/卸载/下载 三类确认框**标题统一 16px** —— 应用名长短不一,
 *   24px 会挤成两行; 16px 一行放得下, 三类弹窗规格也一致。 */
static void am_dlg_title16(ui_ctx_t *ctx) {
    os_dlg_stack_t *d = os_dialog_top_mut(ctx);
    if (d) d->title_small = true;
}

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
    /* ★★★ 2026-10-18 定案: **回调里什么都不做** —— 框架(os_dialog)在回调返回后
     *   会自己关掉这个确认框(dlg_finish_top)。手动 pop 反而多退一层 ⇒
     *   表现就是"显示已卸载 ⇒ 直接回主桌面" ✗ (os_dialog_pop 在 s_top==0 时会 os_pop 弹窗页) */
    ctx->needs_redraw = true;
}
/* ========== 商店 云端包: 先下载到商店源目录, 再走普通安装 (2026-10-04) ==========
 * 下载成功 → 文件落在 /sdcard/store_pkg (或 SPIFFS /store 根) → 与用户自己拷进去的包
 * **完全等价**, 重扫后就从"云端条目"变成"本地条目", 安装逻辑一行都不用改。 */
static void am_store_dl_cb(ui_ctx_t *ctx, int result, void *ud) {
    int rk = (int)(long)ud;
    if (result != 0) { am_rebuild_keep(); return; }   /* 取消: 弹窗由框架自动关 */
    int rpos = (rk >= 0 && rk < s_rview_n) ? s_rview[rk].ridx : -1;
    /* ★★★ 2026-10-18 彻底修 (linit 反复报"无法开始下载"):
     *   原来直接用**缓存的 ridx** —— 索引刷新后(尤其失败回退内置索引)包顺序会变,
     *   ridx 就指向错的包或越界 ⇒ `am_rstore_dl_start()` 立刻拒绝 ✗
     *   改为**按文件名重新查找当前索引里真正的下标** ✓ */
    if (rpos < 0 || rpos >= am_rstore_count()) {
        const char *fn = (rk >= 0 && rk < s_rview_n) ? s_rview[rk].fname : NULL;
        rpos = -1;
        if (fn && fn[0]) {
            char stem[48];
            snprintf(stem, sizeof(stem), "%s", fn);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = 0;
            for (int i = 0; i < am_rstore_count(); i++) {
                const am_rpkg_t *rp = am_rstore_get(i);
                if (rp && strcmp(rp->name, stem) == 0) { rpos = i; break; }
            }
            ESP_LOGI("AMSTORE", "按文件名重定位: %s -> idx %d (共 %d 个包)", stem, rpos, am_rstore_count());
        }
    }
    if (rpos < 0 || !am_rstore_dl_start(rpos))
        os_dialog_toast(ctx, "\xe6\x97\xa0\xe6\xb3\x95\xe5\xbc\x80\xe5\xa7\x8b\xe4\xb8\x8b\xe8\xbd\xbd"); /* 无法开始下载 */
    am_rebuild_keep();
    /* 同上: 交给框架自己关, 不要手动 pop */
    ctx->needs_redraw = true;
}
/* 云端条目被点 → 弹"下载 XX (123KB)?" */
static void am_store_download_ask(ui_ctx_t *ctx, int rk) {
    if (rk < 0 || rk >= s_rview_n) return;
    /* ★★ 2026-10-18 linit: 未联网时**直接提示"请连接网络"**，不再弹"下载 XX?"。
     *   原来的问题是: 先弹确认 → 点确定 → dl_start 被拒 → 只弹一句"无法开始下载"，
     *   用户以为卡住了。这里提前拦掉，少一次无意义点击。 */
    if (!wifi_manager_is_connected()) {
        os_dialog_toast(ctx, "\xe8\xaf\xb7\xe8\xbf\x9e\xe6\x8e\xa5\xe7\xbd\x91\xe7\xbb\x9c"); /* 请连接网络 */
        return;
    }
    const am_rpkg_t *rp = am_rstore_get(s_rview[rk].ridx);
    char title[72];
    if (rp)
        snprintf(title, sizeof(title), "\xe4\xb8\x8b\xe8\xbd\xbd %s?", rp->label);   /* 下载 XX? (不再带大小) */
    else
        snprintf(title, sizeof(title), "\xe4\xb8\x8b\xe8\xbd\xbd %s?", s_rview[rk].label);
    os_dialog_confirm_ex(ctx, title, 0, 0, am_store_dl_cb, (void *)(long)rk);
    /* ★ 只有这个弹窗用 16px 标题(名字长也不挤), 其他弹窗保持 24px ✓ */
    am_dlg_title16(ctx);
}

/* ★ 更新: 已装旧版 + 索引里有更高 ver → 走与「下载」完全相同的通道。
 *   包落到 /sdcard/store_pkg/<name>.lua，下载完成后 am_store_net.c 会调
 *   am_store_auto_install() 把它覆盖安装掉（那里用 tmp + 改名，失败保留旧版）。
 *   不再弹二次确认：按钮已经明确写着「更新」，再确认一次只是多一次点击。 */
static void am_store_update_start(ui_ctx_t *ctx, int k) {
    if (k < 0 || k >= s_pkg_n) return;
    if (!wifi_manager_is_connected()) {
        os_dialog_toast(ctx, "\xe8\xaf\xb7\xe8\xbf\x9e\xe6\x8e\xa5\xe7\xbd\x91\xe7\xbb\x9c"); /* 请连接网络 */
        return;
    }
    char bn[48];
    snprintf(bn, sizeof(bn), "%s", s_pkg[k].fname);
    char *dot = strrchr(bn, '.');
    if (dot) *dot = 0;
    int rpos = -1;
    for (int i = 0; i < am_rstore_count(); i++) {
        const am_rpkg_t *rp = am_rstore_get(i);
        if (rp && rp->name[0] && strcmp(rp->name, bn) == 0) { rpos = i; break; }
    }
    if (rpos < 0) {
        os_dialog_toast(ctx, "\xe7\xb4\xa2\xe5\xbc\x95\xe7\xbc\xba\xe5\xa4\xb1");   /* 索引缺失 */
        return;
    }
    /* ★ 先清掉商店源目录里的旧副本 —— auto_install 的"源"就是它，
     *   留着旧的会"安装成功但还是老版本"，等于没更新。 */
    char stale[192];
    snprintf(stale, sizeof(stale), "%s/%s", AM_STORE_SRC2, s_pkg[k].fname);
    remove(stale);
    ESP_LOGI("AMSTORE", "更新: %s v%u -> v%u", s_pkg[k].fname,
             (unsigned)(s_pkg[k].ver ? s_pkg[k].ver : 1), (unsigned)s_pkg[k].rver);
    if (!am_rstore_dl_start(rpos)) {
        os_dialog_toast(ctx, "\xe6\x97\xa0\xe6\xb3\x95\xe5\xbc\x80\xe5\xa7\x8b\xe4\xb8\x8b\xe8\xbd\xbd"); /* 无法开始下载 */
        return;
    }
    os_dialog_toast(ctx, "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x9b\xb4\xe6\x96\xb0...");   /* 正在更新... */
    am_rebuild_keep();
}

/* ★ 商店行统一入口 (2026-10-02 linit: "商店只负责安装/卸载, 卸载不负责启动应用"):
 *   已安装 → 弹"卸载 XXX?" ; 未安装 → 弹"安装 XXX?" ; 云端未下载 → 弹"下载 XXX?" 。
 *   **商店里永远不启动应用** —— 要运行请到「应用」分类点它。
 *   (原来已安装时走这里会直接运行, 用户点了按钮却跑起来、卸载还藏在长按里, 等于点不到) */

/* ================= 资源包 (type=asset) 助手 (2026-10-25 linit) =================
 * 资源包没有 .lua 文件可扫, 所以"装没装"只能看 target 目录在不在。
 * 装完 → 商店里按钮变"卸载", 再点就递归删目录。 */
static void am_store_asset_uninstall_ask(ui_ctx_t *ctx, const os_app_t *a);

/* ★★★ 2026-10-25 linit「仙剑还是不能点卸载」的根因修复：
 *   原来只认"条目指针来自 s_rview"这一种情况 ⇒ 同一个资源包有两种行：
 *     ① 商店里的云端行（来自 s_rview）      → 认
 *     ② 注册表里的 C 应用行（如 page_pal）  → **认不出**，按钮永远"下载"、点了没反应
 *   现在统一按"资源包身份"找，与行从哪来无关。 */
static int am_asset_pkg_find(const os_app_t *a)
{
    if (!a) return -1;
    int r = am_rview_find(a);                 /* ① 云端行 */
    if (r >= 0) {
        int rid = s_rview[r].ridx;
        const am_rpkg_t *rp = am_rstore_get(rid);
        if (rp && strcmp(rp->type, "asset") == 0 && rp->target[0]) return rid;
    }
    if (am_rstore_count() == 0) return -1;

    /* ② 注册表行：按 label / id / 包名词干(去掉 .zip 与目录) 匹配 */
    char stem_id[48];
    snprintf(stem_id, sizeof(stem_id), "%s", a->id ? a->id : "");
    for (int i = 0; i < am_rstore_count(); i++) {
        const am_rpkg_t *rp = am_rstore_get(i);
        if (!rp || strcmp(rp->type, "asset") != 0 || !rp->target[0]) continue;
        if (a->label && a->label[0] && strcmp(rp->name, a->label) == 0) return i;
        if (a->label && a->label[0] && strcmp(rp->label, a->label) == 0) return i;
        if (stem_id[0] && strcmp(rp->name, stem_id) == 0) return i;
        const char *base = strrchr(rp->path, '/');
        base = base ? base + 1 : rp->path;
        char stem[48];
        snprintf(stem, sizeof(stem), "%s", base);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        if (stem_id[0] && strcmp(stem, stem_id) == 0) return i;
    }
    return -1;
}

static bool am_asset_target_of(const os_app_t *a, char *out, size_t cap)
{
    int i = am_asset_pkg_find(a);
    if (i < 0) return false;
    const am_rpkg_t *rp = am_rstore_get(i);
    if (!rp) return false;
    snprintf(out, cap, "%s", rp->target);
    return true;
}

/* 2026-10-30: 取该资源包的 zip 相对路径 (pkgs/xxx.zip) —— 卸载要按它找清单 */
static bool am_asset_zip_of(const os_app_t *a, char *out, size_t cap)
{
    int i = am_asset_pkg_find(a);
    if (i < 0) return false;
    const am_rpkg_t *rp = am_rstore_get(i);
    if (!rp || !rp->path[0]) return false;
    snprintf(out, cap, "%s", rp->path);
    return true;
}

static bool am_asset_installed(const os_app_t *a)
{
    char dir[96];
    if (!am_asset_target_of(a, dir, sizeof(dir))) return false;

    /* 包名（zip 主干）—— 字体包的 target 是 13 个包共用的 /sdcard/fonts，
     * 一切判定都必须**按包**做，不能按目录做。 */
    char zip[128] = {0}, stem[64] = {0};
    if (!am_asset_zip_of(a, zip, sizeof(zip))) return false;
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

    /* ① 安装标记必须存在。兼容旧版：老实现只写目录级 `<target>/.installed`，
     *   那种标记对共享目录来说"装一个=全装"，**单独存在不算数**，
     *   必须再配上本包自己的清单才认（下面统一按清单回验）。 */
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

    /* ② 按清单逐个回验：文件在不在、大小是否 > 0、.lbf 魔数对不对。
     *   —— 这一步就是 linit 要求的"本地文件确实存在 + 大小正确 + 格式解析成功"。
     *   用户删了 .lbf、卡被拔、文件被别的包覆盖，都会在这里被判否 ⇒ 回到"下载"。 */
    FILE *lf = fopen(lst, "r");
    if (!lf) return false;

    int  cnt = 0;
    unsigned long long bytes = 0;
    char line[288];
    while (fgets(line, sizeof(line), lf)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (line[0] == '/' || strstr(line, "..")) { fclose(lf); return false; }   /* 清单被改坏 */

        char fp[336];
        snprintf(fp, sizeof(fp), "%s/%s", dir, line);
        struct stat st;
        if (stat(fp, &st) != 0 || st.st_size <= 0) { fclose(lf); return false; }  /* 文件丢失 */

        /* 字体文件还要过魔数：挡住"同名空壳/被截断/被换成别的东西" */
        const char *dot = strrchr(line, '.');
        if (dot && (strcmp(dot, ".lbf") == 0 || strcmp(dot, ".LBF") == 0)) {
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
    if (want_cnt >= 0 && cnt != want_cnt) return false;          /* 数量对不上 */
    if (has_bytes && bytes != want_bytes) return false;          /* 总字节对不上 */
    return true;
}

void am_store_toggle(ui_ctx_t *ctx, const os_app_t *a) {
    if (!a) return;
    /* 资源包 (下载 zip + 解压到 target): 装没装看目录, 双态都是同一条行为 */
    if (am_asset_pkg_find(a) >= 0) {
        if (am_asset_installed(a)) { am_store_asset_uninstall_ask(ctx, a); return; }
        int rk = am_rview_find(a);
        if (rk >= 0) am_store_download_ask(ctx, rk);
        return;
    }
    int rk = am_rview_find(a);
    if (rk >= 0) {
        am_store_download_ask(ctx, rk); return;   /* 云端包: 先下载 */
    }
    int k = am_pkg_find(a);
    if (k < 0) return;
    if (am_pkg_has_update(k)) { am_store_update_start(ctx, k); return; }  /* ★ 有新版本 → 直接更新 */
    if (am_pkg_installed(k)) { am_store_uninstall_ask(ctx, a); return; }
    am_store_activate(ctx, a);          /* 未装时 activate 的本职就是弹"安装?"确认 */
}

/* ========== 商店行「详情」弹窗 (2026-10-31) ==========
 * 点列表行的**非按钮区** → 弹这个；点右侧按钮才执行下载/安装/卸载。
 * 数据源: 云端索引 am_rpkg_t (有 desc/ver/size/author/date) 优先, 本地包兜底。
 * 承载用 os_dialog_list: 每行一个字段, 点任意行即关闭 (不引入新弹窗类型)。 */
static void am_store_detail_cb(ui_ctx_t *ctx, int r, void *ud) {
    (void)ctx; (void)r; (void)ud;      /* 框架在回调返回后自动关闭 */
}

void am_store_detail(ui_ctx_t *ctx, const os_app_t *a) {
    if (!a) return;
    int rk = am_rview_find(a);
    int k  = am_pkg_find(a);
    const am_rpkg_t *rp = (rk >= 0) ? am_rstore_get(s_rview[rk].ridx) : NULL;

    char buf[7][96];
    const char *ptrs[7];
    int n = 0;

    if (a->cat && a->cat[0]) {
        snprintf(buf[n], sizeof(buf[n]), "\xe5\x88\x86\xe7\xb1\xbb: %s", a->cat);  /* 分类: */
        ptrs[n] = buf[n]; n++;
    }
    if (rp) {
        snprintf(buf[n], sizeof(buf[n]), "\xe7\x89\x88\xe6\x9c\xac: %u", (unsigned)rp->ver); /* 版本: */
        ptrs[n] = buf[n]; n++;
        if (rp->author[0]) {
            snprintf(buf[n], sizeof(buf[n]), "\xe4\xbd\x9c\xe8\x80\x85: %s", rp->author);     /* 作者: */
            ptrs[n] = buf[n]; n++;
        }
        if (rp->size) {
            snprintf(buf[n], sizeof(buf[n]), "\xe5\xa4\xa7\xe5\xb0\x8f: %u KB",             /* 大小: */
                     (unsigned)((rp->size + 1023u) / 1024u));
            ptrs[n] = buf[n]; n++;
        }
        if (rp->date[0]) {
            snprintf(buf[n], sizeof(buf[n]), "\xe6\x97\xa5\xe6\x9c\x9f: %s", rp->date);      /* 日期: */
            ptrs[n] = buf[n]; n++;
        }
    }
    bool installed = (am_asset_pkg_find(a) >= 0) ? am_asset_installed(a)
                    : (k >= 0 && am_pkg_installed(k));
    snprintf(buf[n], sizeof(buf[n]), "\xe7\x8a\xb6\xe6\x80\x81: %s",                          /* 状态: */
             installed ? "\xe5\xb7\xb2\xe5\xae\x89\xe8\xa3\x85"                                /* 已安装 */
                       : "\xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85");                              /* 未安装 */
    ptrs[n] = buf[n]; n++;

    const char *ds = am_store_sub_of(a);
    if (ds && ds[0] && n < 7) {
        snprintf(buf[n], sizeof(buf[n]), "%s", ds);
        ptrs[n] = buf[n]; n++;
    }
    if (n == 0) return;
    os_dialog_list(ctx, a->label, ptrs, n, 0, am_store_detail_cb, NULL);
    am_dlg_title16(ctx);   /* 名字可能长, 标题用 16px 不挤 (与下载确认一致) */
}

/* 商店选中: 已安装(内置或卡) → 运行; 未安装 → 弹"安装 XXX?" 确认 (目标是应用默认, 不选) */
void am_store_activate(ui_ctx_t *ctx, const os_app_t *a) {
    if (am_asset_pkg_find(a) >= 0) {
        if (am_asset_installed(a)) { am_store_asset_uninstall_ask(ctx, a); return; }
        int rk0 = am_rview_find(a);
        if (rk0 >= 0) am_store_download_ask(ctx, rk0);
        return;
    }
    int rk = am_rview_find(a);
    if (rk >= 0) {
        am_store_download_ask(ctx, rk); return;   /* 云端包: 先下载 */
    }
    int k = am_pkg_find(a);
    if (k < 0) return;
    if (am_pkg_has_update(k)) { am_store_update_start(ctx, k); return; }  /* ★ 有新版本 → 直接更新 */
    if (am_pkg_installed(k)) {
        /* ★★★ 2026-10-18 linit 反复报"点卸载就退出应用中心"的真因:
         *   商店里点**已安装的行**原来会 `am_lua_open_path()` **直接启动应用** ✗
         *   ⇒ 用户以为"退出应用中心" ✓ (实际是被拉去跑那个应用了)
         *   商店里**永远不启动应用**(只负责装/卸) ⇒ 已安装的点一下就弹"卸载?" ✓ */
        am_store_uninstall_ask(ctx, a);
        return;
    }
    char title[64];
    snprintf(title, sizeof(title), "\xe5\xae\x89\xe8\xa3\x85 %s ?", s_pkg[k].label); /* 安装 XXX ? */
    os_dialog_confirm_ex(ctx, title, 0, 0, am_store_install_cb, (void *)(long)k);
    am_dlg_title16(ctx);
}

/* ========== 商店 卸载 (删除已装包 + 移出主菜单 + 重新扫描) ========== */
/* ★★★ 2026-10-18 linit: **卸载放后台任务** —— 原来在 UI 线程删 4 个文件 + 写两次 NVS,
 *   会卡住界面。现在 UI 立刻弹 toast, 文件删除/记录清理在后台做完再刷新列表。 */
typedef struct {
    char paths[4][192];
    char lid[64];
    int  npaths;
} am_uninst_job_t;

static void am_uninst_task(void *arg) {
    am_uninst_job_t *j = (am_uninst_job_t *)arg;
    for (int i = 0; i < j->npaths; i++)
        if (j->paths[i][0]) remove(j->paths[i]);
    os_app_set_mainmenu(j->lid, false);
    os_app_set_hidden(j->lid, true);
    /* ★★ 2026-10-18: **卸载后不再触发全量重扫** —— 文件已删, `am_store_installed_of()`
     *   按文件存在性判断, 自动变"下载"; 重扫只会拖慢(linit 反馈"卸载慢") ✗
     *   只让 Lua 列表失效(应用管家里那条要消失) ✓ */
    am_lua_invalidate();
    ESP_LOGI("AMSTORE", "后台卸载完成: %s (删了 %d 个文件)", j->lid, j->npaths);
    s_uninst_done = true;    /* ★ 通知 UI 刷新(它只重绘, 不重建列表) */

    free(j);
    vTaskDelete(NULL);
}

static bool s_uninst_pending = false;   /* 有卸载任务在跑 (UI 提示用) */
static void am_store_uninstall_do(ui_ctx_t *ctx, int k) {
    if (k < 0 || k >= s_pkg_n) return;
    if (!am_pkg_installed(k)) { os_dialog_toast(ctx, "\xe6\x9c\xaa\xe5\xae\x89\xe8\xa3\x85"); return; } /* 未安装 */
    /* ★★★ 2026-10-18: **不再用"忙标志"挡** —— 之前任务异常退出会把标志永久置 true
     *   ⇒ "点卸载没反应" ✗。卸载是幂等的(删不存在的文件只返回错误),
     *   连点/重复完全无害 ⇒ 直接放行, 由后台任务各删各的 ✓ */
    am_uninst_job_t *j = calloc(1, sizeof(*j));
    if (!j) return;

    /* 先把要删的路径拷进任务(任务跑的时候 s_pkg 可能已被重扫 ✗)
     * ★ 2026-10-27: dst/dst_sd 现在由扫描显式填好(内置/ TF 两条候选路径),
     *   直接照抄即可 —— 不再用 cat 现拼, 免得 cat 与实际所在目录不一致时删错路径。 */
    if (s_pkg[k].dst[0])    snprintf(j->paths[j->npaths++], 192, "%s", s_pkg[k].dst);
    if (s_pkg[k].dst_sd[0]) snprintf(j->paths[j->npaths++], 192, "%s", s_pkg[k].dst_sd);
    snprintf(j->paths[j->npaths++], 192, "/sdcard/store_pkg/%s", s_pkg[k].fname);
    snprintf(j->paths[j->npaths++], 192, "/store/%s", s_pkg[k].fname);
    snprintf(j->lid, sizeof(j->lid), "lua:%s", s_pkg[k].fname);
    s_pkg_inst[k] = false;   /* UI 立刻认为未安装(按钮变"下载") */
    /* ★ 2026-10-30: 原来这里 os_dialog_toast("卸载中...") —— 但 toast 会把 s_cb_handled
     *   置 true, 框架就不关确认框了 ⇒ 弹窗残留挡在商店上。
     *   这里改成"不弹 toast", 靠 s_pkg_inst[k]=false 让按钮立刻变回"下载"作为反馈;
     *   后台删完会 am_rebuild_keep() 刷新列表。 */
    s_uninst_pending = true;
    ctx->needs_redraw = true;
    if (xTaskCreate(am_uninst_task, "am_uninst", 6144, j, 3, NULL) != pdPASS) {
        /* 任务建不起来: 退化为同步删(至少保证功能正确) */
        am_uninst_task(j);
    }
}

static void am_store_uninstall_cb(ui_ctx_t *ctx, int result, void *ud) {
    /* ★★★ 2026-10-30 修「卸载确认后整个应用中心退出」的**真正根因**:
     *   dlg_finish_top() (os_dialog.c:521) 的顺序是: 先 clear s_cb_handled → 调 cb
     *   → cb 返回后才 os_dialog_pop。也就是说 **cb 执行时 s_top 还是 0**。
     *   而 os_dialog_pop() 在 s_top==0 时走的是 `os_pop(ctx)` —— 直接退出弹窗页/商店页!
     *   (os_dialog_depth() = s_top+1 = 1, 所以 `> 0` 永远成立 → 每次都退页面)
     *   ⇒ 回调里**绝对不能**调 os_dialog_pop。
     *   关窗交给框架的 dlg_finish_top 完成 (s_cb_handled 保持 false 即可)。 */
    if (result != 0) { ctx->needs_redraw = true; return; }   /* 取消: 只关框, 不卸载 */
    am_store_uninstall_do(ctx, (int)(long)ud);
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
    am_dlg_title16(ctx);
}

/* ---------- 资源包卸载 ----------
 * ★ 2026-10-30: 优先「按清单只删本包的文件」。
 *   字体包的 target 是 /sdcard/fonts（多包共用 + 用户手动放的字体也在这），
 *   老实现递归删目录 ⇒ 卸一个字体把所有人一起清掉。
 *   只有拿不到清单的旧包（仙剑那种独占目录）才回退到整目录删。 */
typedef struct { char zip[128]; char dir[96]; } asset_rm_arg_t;

static void am_asset_uninst_task(void *arg)
{
    asset_rm_arg_t *p = (asset_rm_arg_t *)arg;
    bool ok = am_asset_uninstall(p->zip, p->dir);
    if (ok) {
        ESP_LOGI("AMSTORE", "按清单卸载 %s: OK", p->dir);
    } else {
        ok = am_asset_rmdir(p->dir);
        ESP_LOGI("AMSTORE", "资源包卸载(整目录) %s: %s", p->dir, ok ? "OK" : "失败");
    }
    free(p);
    s_uninst_done = true;    /* 通知 UI 刷新(按钮回到"下载") */
    vTaskDelete(NULL);
}

static void am_store_asset_uninstall_cb(ui_ctx_t *ctx, int result, void *ud)
{
    asset_rm_arg_t *dir = (asset_rm_arg_t *)ud;
    if (os_dialog_depth() > 0) os_dialog_pop(ctx);
    if (result == 0 && dir) {
        asset_rm_arg_t *copy = (asset_rm_arg_t *)malloc(sizeof(*copy));
        if (copy) *copy = *dir;
        free(dir);
        if (copy) {
            if (xTaskCreate(am_asset_uninst_task, "am_asset_rm", 4096, copy, 3, NULL) != pdPASS) {
                am_asset_uninst_task(copy);   /* 建不出任务就同步删 */
            }
        }
        os_dialog_toast(ctx, "\xe5\x8d\xb8\xe8\xbd\xbd\xe4\xb8\xad...");   /* 卸载中... */
    } else {
        free(dir);
    }
    ctx->needs_redraw = true;
}

static void am_store_asset_uninstall_ask(ui_ctx_t *ctx, const os_app_t *a)
{
    char dir[96], zip[128] = {0};
    if (!am_asset_target_of(a, dir, sizeof(dir))) return;

    /* ★★★ 2026-10-30: 正在被阅读器使用的字体**不许卸**。
     *   删掉当前字库后，当前这一页会立刻变成一排缺字框，用户还以为把阅读器弄坏了。
     *   判断方式很轻: 阅读器暴露"当前 LBF 绝对路径"，这里做一次目录前缀匹配，
     *   不引入 apps→engines 的新依赖（apps 本来就 REQUIRES book_reader）。
     *   用户切到别的字体（或切回内置）后，路径变了，就能正常卸了。 */
    {
        const char *using_f = font_book_current_file();
        if (using_f && using_f[0]) {
            /* 先按**本包的安装清单**核对: 正在用的字体到底是不是这个包装进来的。
             * 只比清单而不是"目录前缀" —— /sdcard/fonts 下通常装了好几个字体，
             * 用前缀判断会把"卸载别的字体"也拦下来。
             * 旧包没有清单(has_file 返回 false)时退回前缀判断，宁可保守也不误删。 */
            bool mine = zip[0] && am_asset_has_file(zip, dir, using_f);
            if (!mine) {
                size_t dl = strlen(dir);
                bool under = (strncmp(using_f, dir, dl) == 0 &&
                              (using_f[dl] == '/' || using_f[dl] == 0));
                /* 只有"整个目录就是这一个包"时才用前缀兜底(仙剑那种独占目录) */
                mine = under && !zip[0];
            }
            if (mine) {
                ESP_LOGW("AMSTORE", "拒绝卸载: 字体正在使用中 (%s)", using_f);
                os_dialog_toast(ctx, "\xe8\xaf\xa5\xe5\xad\x97\xe4\xbd\x93\xe6\xad\xa3\xe5\x9c\xa8\xe4\xbd\xbf\xe7\x94\xa8\xef\xbc\x8c\xe8\xaf\xb7\xe5\x85\x88\xe5\x88\x87\xe6\x8d\xa2\xe5\xad\x97\xe4\xbd\x93");
                return;
            }
        }
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "\xe5\x8d\xb8\xe8\xbd\xbd %s?", a->label);   /* 卸载 XX? */
    asset_rm_arg_t *ud = (asset_rm_arg_t *)calloc(1, sizeof(*ud));
    if (!ud) return;
    snprintf(ud->dir, sizeof(ud->dir), "%s", dir);
    if (!am_asset_zip_of(a, zip, sizeof(zip))) zip[0] = 0;
    snprintf(ud->zip, sizeof(ud->zip), "%s", zip);
    os_dialog_confirm_ex(ctx, msg, 0, 0, am_store_asset_uninstall_cb, ud);
    am_dlg_title16(ctx);
}

/* ==================== 对外接口 (声明见 am_internal.h) ==================== */
bool am_store_ready(void) { return s_pkg_scanned == 0; }
/* ★ 2026-10-18: 取"卸载完成"事件(取一次即清), UI 用它触发一次重绘 ✓ */
bool am_store_take_uninst_done(void) {
    if (!s_uninst_done) return false;
    s_uninst_done = false;
    return true;
}
int  am_store_count(void) { return am_total_n(); }

const os_app_t *am_store_app(int k) {
    const am_pkg_entry_t *e = am_entry(k);
    return e ? &e->app : NULL;
}
bool am_store_owns(const os_app_t *a) { return am_pkg_find(a) >= 0 || am_rview_find(a) >= 0; }
bool am_store_installed_of(const os_app_t *a) {
    int k = am_pkg_find(a);
    /* 读缓存: 渲染路径不能逐帧 fopen。普通云端包 (kind=1) 不算"已装"—— 还没下载呢。
     * ★ 2026-10-25: **资源包例外** —— 它没有 .lua 可扫, 装完就靠 target 目录在不在。
     *   (渲染每帧调一次, stat 一个目录比 fopen 便宜得多; 目录不存在时立即返回) */
    if (k >= 0) return s_pkg_inst[k];
    return am_asset_installed(a);
}
const char *am_store_fname_of(const os_app_t *a) {
    int k = am_pkg_find(a);
    if (k >= 0) return s_pkg[k].fname;
    int r = am_rview_find(a);
    return (r >= 0) ? s_rview[r].fname : NULL;
}
/* 商店行第二行文案: 本地包 → LAPP 简介 (可能 NULL → 不画);
 *                    云端包 → "未下载 · NKB" (让用户知道点了要下多少) */
/* ★ 2026-10-18 linit: 该条目正在下载时返回进度(0..1000), 否则 -1。
 *   行渲染据此把第二行画成进度条(顶掉应用说明)。 */
int am_store_dl_permille_of(const os_app_t *a) {
    const char *nm = NULL;
    int kk = am_pkg_find(a);
    if (kk >= 0) nm = s_pkg[kk].fname;
    else { int rr = am_rview_find(a); if (rr >= 0) nm = s_rview[rr].fname; }
    char stem[48];
    snprintf(stem, sizeof(stem), "%s", nm ? nm : "");
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    const char *dl = am_rstore_dl_name();
    if (!dl || !dl[0] || strcmp(stem, dl) != 0) return -1;
    int pm = am_rstore_dl_permille();
    return (pm < 0) ? 0 : pm;
}

const char *am_store_sub_of(const os_app_t *a) {
    /* ★★★ 2026-10-18 linit: **正在下载的那一条, 第二行临时显示进度**(顶掉应用说明) ——
     *   不再靠弹窗。返回静态缓冲(渲染路径每帧调, 不能 malloc)。 */
    {
        const char *nm = NULL;
        int kk = am_pkg_find(a);
        if (kk >= 0) nm = s_pkg[kk].fname;
        else { int rr = am_rview_find(a); if (rr >= 0) nm = s_rview[rr].fname; }
        const char *dl = am_rstore_dl_name();
        /* ★ 2026-10-18 修: 条目 fname 带 ".lua", dl_name 不带 ⇒ 去掉后缀再比, 否则进度永远不显示 */
        char stem[48];
        snprintf(stem, sizeof(stem), "%s", nm ? nm : "");
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        if (nm && dl && dl[0] && strcmp(stem, dl) == 0) {
            static char s_pg[28];
            int pm = am_rstore_dl_permille();
            if (pm < 0) snprintf(s_pg, sizeof(s_pg), "\xe4\xb8\x8b\xe8\xbd\xbd\xe4\xb8\xad ...");   /* 下载中 ... */
            else        snprintf(s_pg, sizeof(s_pg), "\xe4\xb8\x8b\xe8\xbd\xbd\xe4\xb8\xad %d%%", pm / 10); /* 下载中 42% */
            return s_pg;
        }
    }
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
    /* ★★★ 2026-10-25 linit「装完还是显示下载」的根因：原来只要行来自云端就
     *   无条件返回"下载"，资源包装好了也不变 ⇒ 用户永远看不到"卸载"。
     *   资源包必须双态：目录在 = 卸载，不在 = 下载。 */
    if (am_asset_pkg_find(a) >= 0)
        return am_asset_installed(a) ? "\xe5\x8d\xb8\xe8\xbd\xbd"        /* 卸载 */
                                     : "\xe4\xb8\x8b\xe8\xbd\xbd";       /* 下载 */
    if (am_rview_find(a) >= 0) return "\xe4\xb8\x8b\xe8\xbd\xbd";        /* 下载 */
    /* ★★★ 2026-10-27 linit 定：**有新版就把按钮变成「更新」**，否则保持「卸载」。
     *   只在真推了新版本(rver > ver)时才变，平时语义不变，老用户不会被搞混。
     *   卸载仍然可用（长按行 → 卸载确认），不会因为变成「更新」就丢了卸载入口。 */
    { int ku = am_pkg_find(a); if (ku >= 0 && am_pkg_has_update(ku)) return "\xe6\x9b\xb4\xe6\x96\xb0"; /* 更新 */ }
    /* ★ 2026-10-27 linit 定的全局规则: 商店里**只有两种动作** ——
     *   未安装 = "下载"(下载完成自动安装, 本地已有包则直接安装)
     *   已安装 = "卸载"(把 TF 与内置分区一起删干净)
     *   不再出现"安装"这个中间态(它让用户以为已经装了却要再点一次)。 */
    return am_store_installed_of(a) ? "\xe5\x8d\xb8\xe8\xbd\xbd"          /* 卸载 */
                                    : "\xe4\xb8\x8b\xe8\xbd\xbd";         /* 下载 */
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
        s_rver_gen = -1;          /* ★ 条目集合变了 → 重新对齐云端版本 */
        changed = true;
    }
    int before = s_rview_n;
    am_store_sync_remote();
    if (s_rview_n != before) changed = true;
    /* ★ 2026-10-27: 索引是异步到达的(扫描时可能还没有)，到齐后补一次版本对齐，
     *   否则「更新」按钮要等下次重进商店才出现。条数没变就整段跳过，几乎零开销。 */
    if (am_rstore_count() != s_rver_gen) {
        am_sync_rver();
        changed = true;
    }
    return changed;
}

/* ===== 2026-10-31: 应用在「TF 卡 ↔ 内置」之间移动 ==========================
 * 背景: 内置 apps 分区只有 4MB（SPIFFS 固定开销后可用 ≈3.65MB），
 *   所以新装应用默认一律落 TF 卡（见 am_store_auto_install）。
 *   但用户会希望个别应用"内置化"——不插卡也能开、卡拔了也还在。
 *   入口：商店里长按应用图标（见 page_app_manager.c 的长按分支）。
 * 原则：动之前先算空间，不够就明确拒绝；移动是"复制成功→校验大小→删源"，
 *   中途失败绝不留半个文件，也不会把源弄丢。
 */

/* 0=未安装 1=内置 2=TF 卡 */
int am_store_app_where(const os_app_t *a)
{
    int k = am_pkg_find(a);
    if (k < 0) return 0;
    if (s_pkg[k].dst_sd[0] && am_file_exist(s_pkg[k].dst_sd)) return 2;
    if (s_pkg[k].dst[0]    && am_file_exist(s_pkg[k].dst))    return 1;
    return 0;
}

/* apps 分区剩余字节（SPIFFS）；查询失败返回 0 */
uint32_t am_store_internal_free(void)
{
    size_t total = 0, used = 0;
    if (esp_spiffs_info("apps", &total, &used) != ESP_OK) return 0;
    return (used >= total) ? 0u : (uint32_t)(total - used);
}

/* 把 TF 卡上的应用移到内置。成功 true；失败 err 里是中文原因。 */
bool am_store_move_to_internal(const os_app_t *a, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    int k = am_pkg_find(a);
    if (k < 0) { if (err) snprintf(err, errlen, "找不到应用记录"); return false; }
    if (!s_pkg[k].dst_sd[0] || !am_file_exist(s_pkg[k].dst_sd)) {
        if (err) snprintf(err, errlen, "不在 TF 卡上");
        return false;
    }
    if (s_pkg[k].dst[0] && am_file_exist(s_pkg[k].dst)) {
        if (err) snprintf(err, errlen, "内置已有一份");
        return false;
    }

    struct stat st;
    if (stat(s_pkg[k].dst_sd, &st) != 0 || st.st_size <= 0) {
        if (err) snprintf(err, errlen, "源文件读不到");
        return false;
    }
    const uint32_t need = (uint32_t)st.st_size;

    uint32_t freeb = am_store_internal_free();
    /* 预留 8KB：SPIFFS 的块/目录开销，卡着满容量写必然失败 */
    if (freeb < need + 8192u) {
        if (err) snprintf(err, errlen, "内置空间不足(需%uKB 余%uKB)",
                          (unsigned)(need / 1024), (unsigned)(freeb / 1024));
        ESP_LOGW("AMSTORE", "移动到内置被拒: 需 %u B, 内置余 %u B", (unsigned)need, (unsigned)freeb);
        return false;
    }

    /* 父目录（/store/apps/<sub>）可能还不存在 */
    char pdir[192];
    snprintf(pdir, sizeof(pdir), "%s", s_pkg[k].dst);
    char *slash = strrchr(pdir, '/');
    if (slash) { *slash = 0; mkdir(pdir, 0755); }

    if (!am_copy_file(s_pkg[k].dst_sd, s_pkg[k].dst)) {
        if (err) snprintf(err, errlen, "复制失败(空间/卡错误)");
        remove(s_pkg[k].dst);
        return false;
    }
    struct stat dt;
    if (stat(s_pkg[k].dst, &dt) != 0 || (long)dt.st_size != (long)st.st_size) {
        if (err) snprintf(err, errlen, "复制校验不符");
        remove(s_pkg[k].dst);                 /* 只删新写的半成品，源仍在卡上 */
        return false;
    }
    remove(s_pkg[k].dst_sd);                  /* 校验通过才删源 */
    ESP_LOGI("AMSTORE", "已移动到内置: %s (%u B, 内置余 %u B)",
             s_pkg[k].dst, (unsigned)need, (unsigned)am_store_internal_free());
    am_store_invalidate();
    am_lua_invalidate();
    return true;
}

/* 把内置的应用移回 TF 卡（反向，给"内置满了想挪出来"留个出口） */
bool am_store_move_to_sd(const os_app_t *a, char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    int k = am_pkg_find(a);
    if (k < 0) { if (err) snprintf(err, errlen, "找不到应用记录"); return false; }
    if (!s_pkg[k].dst[0] || !am_file_exist(s_pkg[k].dst)) {
        if (err) snprintf(err, errlen, "不在内置空间");
        return false;
    }
    if (!sd_is_mounted()) {
        if (err) snprintf(err, errlen, "TF 卡未挂载");
        return false;
    }

    struct stat st;
    if (stat(s_pkg[k].dst, &st) != 0 || st.st_size <= 0) {
        if (err) snprintf(err, errlen, "源文件读不到");
        return false;
    }
    char pdir[192];
    snprintf(pdir, sizeof(pdir), "%s", s_pkg[k].dst_sd);
    char *slash = strrchr(pdir, '/');
    if (slash) { *slash = 0; mkdir(pdir, 0755); }

    if (!am_copy_file(s_pkg[k].dst, s_pkg[k].dst_sd)) {
        if (err) snprintf(err, errlen, "复制到卡失败");
        remove(s_pkg[k].dst_sd);
        return false;
    }
    struct stat dt;
    if (stat(s_pkg[k].dst_sd, &dt) != 0 || (long)dt.st_size != (long)st.st_size) {
        if (err) snprintf(err, errlen, "复制校验不符");
        remove(s_pkg[k].dst_sd);
        return false;
    }
    remove(s_pkg[k].dst);
    ESP_LOGI("AMSTORE", "已移到 TF 卡: %s", s_pkg[k].dst_sd);
    am_store_invalidate();
    am_lua_invalidate();
    return true;
}

/* 长按「移动到内置空间」的确认框。目标指针存 static —— 对话框是异步的，
 * 传栈上的指针/依赖列表索引都可能失效（列表会被重建）。 */
static const os_app_t *s_move_target;

static void am_move_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    const os_app_t *a = s_move_target;
    s_move_target = NULL;
    if (result != 0 || !a) return;          /* 取消 */
    char err[96] = {0};
    if (am_store_move_to_internal(a, err, sizeof(err))) {
        os_dialog_toast(ctx, "已移动到内置空间");
        am_rebuild_keep();
    } else {
        os_dialog_toast(ctx, err[0] ? err : "移动失败");
    }
}

void am_move_internal_ask(ui_ctx_t *ctx, const os_app_t *a)
{
    if (!a) return;
    /* 先算一遍空间，不够就别让用户点进去了（比"点了才报错"友好） */
    uint32_t freeb = am_store_internal_free();
    if (freeb == 0) { os_dialog_toast(ctx, "内置空间不可用"); return; }
    if (freeb < 16u * 1024u) {
        char m[64];
        snprintf(m, sizeof(m), "内置空间不足(余%uKB)", (unsigned)(freeb / 1024));
        os_dialog_toast(ctx, m);
        return;
    }
    s_move_target = a;
    os_dialog_confirm_ex(ctx, "移动到内置空间?", 0, 0, am_move_cb, NULL);
}
