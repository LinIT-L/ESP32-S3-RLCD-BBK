/* ============================================================
 * am_store_json.h — 商店 index.json 的解析 (纯 C, 零 OS 依赖)
 *
 * 为什么单独成文件: 解析器是这一环里**唯一"错了就会静默失效"**的部分
 * (字段名写错 → 商店永远空列表, 不崩不报错)。抽出来 + 不带 os.h 依赖,
 * 就能在宿主机上直接编译单测 (tools/store_parse_test.c), 拿真实 index.json
 * 和一堆畸形输入打一遍, 而不是靠"看着像对的"。
 * ============================================================ */
#ifndef AM_STORE_JSON_H
#define AM_STORE_JSON_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define AM_RPKG_MAX     40
#define AM_MIRROR_MAX   4
#define AM_MIRROR_LEN   256

/* 一条云端包记录 (字段名与 tools/store_build.py 产出的 index.json 一一对应) */
typedef struct {
    char     name[32];    /* 包名 bikecomp (文件名主干) */
    char     label[48];   /* 中文名 (LAPP label=) */
    char     cat[16];     /* app / ops */
    char     desc[96];    /* 商店列表第二行: 应用说明 */
    uint32_t size;        /* 字节数 */
    uint32_t crc;         /* crc32 (zlib/以太网, 由 hex 文本转来) */
    /* ★ 2026-10-27: 版本号 —— 索引里一直有 "ver"(store_build.py 生成), 但这里没字段、
     *   解析器也不读 ⇒ 设备端永远不知道远端版本 ⇒ 商店不可能出现"更新"。
     *   0 = 老索引没这个字段(按"未知"处理, 不提示更新)。 */
    uint32_t ver;
    char     path[96];    /* 相对路径 pkgs/xxx.lua */
    /* ★ 2026-10-25 资源包类 (仙剑这类"只装游戏数据"的包):
     *   type   = "app"(默认, 装 .lua) / "asset"(下载 zip 解到 target)
     *   target = asset 的解包目录, 例如 /sdcard/apps/app/pal
     * 老索引没有这两个字段 ⇒ type 为空, 行为与以前完全一致 (向后兼容)。 */
    char     type[12];
    char     target[80];
    /* ★ 2026-10-27 linit: 统一元信息 —— 作者 / 上架日期。
     *   资源包(type=asset)没有 LAPP 头，只有这里能带这两个字段。 */
    char     author[32];
    char     date[12];
    /* ★ 2026-10-30 linit 发版规则: 更新判定改按**构建时间**。
     *   build = YYYYMMDDHHMM (12 位定长数字), 与 LAPP 头里的 build= 同源。
     *   定长数字串字典序 == 时间先后, 设备端 strcmp 即可。
     *   空 = 老索引没有这个字段 → 退回按 ver 比较 (向后兼容)。 */
    char     build[16];
    /* ★ 2026-11-02 linit: 更新判定**只认服务器文件的实际修改时间**。
     *   mtime = 包文件在服务器上的 mtime (unix 秒, 由 tools/store_build.py 从文件 stat 写入,
     *           不是索引生成时间)。0 = 老索引没有这个字段 ⇒ 设备端退回 size+CRC32 核对。
     *   build 仅用于显示/诊断，不再参与"有没有新版"的判断。 */
    uint32_t mtime;
    /* ★ 2026-11-01: 包级 SHA-256（hex 64 字符）。新协议要求；旧条目为空 ⇒
     *   走"size + CRC32"老规则并在日志里明确标出，不静默降级。 */
    char     sha256[72];
    /* ★ 2026-10-31: 包对**资源包**的显式依赖 (只有主程序需要写, 资源包留空)。
     *   格式 "<zip 路径>[@<最低版本>]", 例: "pkgs/wuxia.zip@1"
     *   为什么需要它: 资源包与主程序本来是"两个独立条目", 安装器靠**名字相同**去配对
     *   (pal / 字体就是这么干的)。像《英雄坛说》这种"主程序名 ≠ 资源包名"的组合,
     *   旧逻辑根本配不上 ⇒ 装完主程序、素材还是缺的, 打开就是白屏。
     *   有了 requires, 安装主程序时会顺带把资源包拉下来, 并能校验版本。
     *   老索引没有这个字段 ⇒ 空串, 行为与以前完全一致。 */
    char     requires[48];
    /* ★ 2026-10-31: 资源包(type=asset)的**文件清单**相对路径, 例如
     *   "pkgs/pal/manifest.json"。有它就走「多文件直接下载」: 设备按清单逐个下
     *   真实文件, 卡上只写一遍, 且天然支持断点续传 —— 不再下 41 MB zip 再解包
     *   (那条路要写两遍, 实测总耗时 385 s 里 133 s 是白写的)。
     *   空 = 老索引/没出清单 ⇒ 完全沿用原来的 ZIP 下载 + 解包, 行为不变。 */
    char     manifest[96];
} am_rpkg_t;

/* ---- 包类型判定（2026-11-01）----
 * 资源包装法现在有两种，上层**不该各写一遍 strcmp**：
 *   "asset" = ZIP 解包（历史路径，字体包仍走它）
 *   "lpk"   = 单文件 LPK，下载即完成，不解压（仙剑 / 英雄坛说）
 * 两者都属于"资源包"：有 target 目录、按包名(stem)记 .installed/.pkg.lst、
 * 卸载走 am_asset_uninstall() —— 所以凡是"是不是资源包"的地方都用 is_res。 */
static inline bool am_pkg_is_res(const char *type) {
    return type && (strcmp(type, "asset") == 0 || strcmp(type, "lpk") == 0);
}
static inline bool am_pkg_is_lpk(const char *type) {
    return type && strcmp(type, "lpk") == 0;
}

/* 解析 index.json。成功返回 0 (且 out_n/mir_n 均 > 0), 失败返回负数:
 *   -1 没解析出 mirrors[]   -2 没有 pkgs[] 键   -3 pkgs[] 里一条有效记录都没有
 *   -4 参数不合法
 * 说明: 单条记录字段不全 → **跳过该条**, 不影响其它条 (服务端加字段/少字段不该让整店瘫痪)。
 * 成功时 out_n / *mir_n 会被写成实际条数。 */
int am_store_json_parse(const char *txt,
                        am_rpkg_t *out, int max, int *out_n,
                        char mir[][AM_MIRROR_LEN], int mir_max, int *mir_n);

/* 供上层日志用: 上一次解析跳过了几条不完整的记录 */
int am_store_json_skipped(void);

#endif /* AM_STORE_JSON_H */
