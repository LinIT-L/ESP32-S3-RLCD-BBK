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
} am_rpkg_t;

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
