/**
 * am_manifest.h — 资源包「文件清单」解析 + 路径守卫 (纯 C, 零 OS 依赖)
 *
 * 为什么要有它
 * ------------
 * 资源包原来是「下一个 41 MB 的 zip, 再在设备上解包」: 卡上要写两遍同样多的字节,
 * 安装耗时 ≈ 下载耗时, 还多一次解压风险。改成「服务器给清单, 设备逐个下载真实文件」后,
 * TF 卡只写一遍, 而且天然支持断点续传(已下好的文件跳过)。
 *
 * 清单格式 (由 tools/mk_asset_manifest.py 自动生成, 不要手写)
 * ---------------------------------------------------------
 *   {
 *     "fmt": 1,                       // 清单格式版本, 必须 = 1
 *     "pkg": "pal",                   // 包名(与 zip 主干同名)
 *     "ver": 3,                       // 包版本
 *     "count": 109,                   // 文件总数
 *     "total": 43720224,              // 所有文件字节数之和 (必须与逐条加总一致)
 *     "files": [
 *       { "path": "ABC.MKF",  "size": 1022564, "crc32": "1a2b3c4d" },
 *       { "path": "sound/x.mkf", "size": 12,   "crc32": "0000beef" }
 *     ]
 *   }
 *
 *   ★ path **必须是相对路径** —— 设备会拼成 <target>/<path>。
 *     绝对路径 / .. 穿越 / 反斜杠 / 盘符 / 控制字符一律在解析阶段拒收,
 *     一条不合规就整份清单作废(不给"部分合法"留口子)。
 *
 * 本文件刻意不 include esp_log.h / os.h —— 宿主机直接编译同一份源码跑单测,
 * 避免"测试副本与生产代码漂移"。
 */
#ifndef AM_MANIFEST_H
#define AM_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AM_MF_PATH_MAX   96     /* 单条相对路径上限(含 NUL); 超长直接判非法 */
#define AM_MF_MAX_FILES  4096   /* 条数上限, 防畸形清单把 PSRAM 吃光 */

typedef struct {
    char     path[AM_MF_PATH_MAX];   /* 相对路径, 例如 "sound/x.mkf" */
    uint32_t size;                   /* 字节数, 必须 > 0 */
    uint32_t crc32;                  /* 与 zlib/binascii.crc32 一致 */
} am_mf_file_t;

typedef struct {
    int      fmt;            /* 清单格式版本 (必须 1) */
    int      nfiles;         /* 文件总数 */
    uint32_t total_bytes;    /* 所有文件大小之和 */
    int      ver;            /* 包版本 (可 0) */
    char     pkg[40];        /* 包名 (可空) */
} am_mf_head_t;

/* 只解析头部(count/total/fmt/ver/pkg), 不碰 files[] —— 调用方据此分配文件表。 */
int  am_mf_head_parse(const char *txt, am_mf_head_t *out);

/* 解析 files[] 到调用方数组。返回条数; 负 = 失败:
 *   -1 参数错  -2 找不到 files[]  -3 条目字段不全/类型错  -4 路径不合法
 *   -5 条数超过 max  -6 单条 size 为 0 */
int  am_mf_files_parse(const char *txt, am_mf_file_t *arr, int max);

/* 路径守卫: 相对、无 .. 段、无反斜杠、无盘符/协议、无控制字符、不以 / 结尾。
 * 这条规则同时被生成器(拒收源数据脏路径)和设备(拒收被改坏的清单)使用。 */
bool am_mf_path_ok(const char *rel);

/* 拼 <dir>/<rel> 到 dst。返回 false = 越界或被守卫拒绝。
 * ★ 所有文件写入都必须经过这里, 别在别处自己 snprintf 拼路径。 */
bool am_mf_join(char *dst, int cap, const char *dir, const char *rel);

/* 校验 head.nfiles 与逐条加总是否与 head.total_bytes 一致。
 * 返回 true = 一致。防止"清单被截断/改坏"导致装出半套资源。 */
bool am_mf_verify_total(const am_mf_file_t *arr, int n, const am_mf_head_t *h);

/* 百分号编码(URL 用): 保留 A-Za-z0-9-_.~/ 其余转 %XX。
 * 与 am_store_net.c 的同源实现一致 —— 中文文件名必须编码, esp_http_client 不做。 */
void am_mf_url_encode(const char *in, char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* AM_MANIFEST_H */
