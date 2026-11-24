/**
 * am_asset_multi.h — 资源包「多文件直接下载」安装器
 *
 * 与 am_asset.c(zip 解包) 的关系
 * -----------------------------
 *   · 老路径: 下 41 MB zip → 解包写 41 MB → 卡上总共写 83 MB, 安装 ≈ 下载耗时。
 *   · 新路径(本文件): 设备按清单逐个下载**真实文件**, 直接落到最终位置。
 *     卡上只写 41 MB, 而且天然支持断点续传(已下好的文件跳过)。
 *
 * 与 am_asset_uninstall() 完全兼容
 * --------------------------------
 *   仍然写 `.pkg.<zip主干>.lst`(相对路径清单) 与 `.installed.<zip主干>`(条数+总字节),
 *   所以卸载/已安装判定一行代码都不用改。
 *
 * 安全契约 (任何一条破了就整份清单作废, 不做"尽力而为")
 * ----------------------------------------------------
 *   ① 相对路径 + 路径守卫(见 am_manifest.h) —— 清单无法让设备写出 target 之外;
 *   ② 每个文件都必须有 size 与 crc32, 下载完由 net_client 在**替换前**校验;
 *   ③ 条数与总字节必须自洽(am_mf_verify_total);
 *   ④ 全部文件成功才写安装标记 —— 中途失败不会留下"看起来装好了"的状态。
 */
#ifndef AM_ASSET_MULTI_H
#define AM_ASSET_MULTI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t done_bytes;    /* 已完成字节(含已跳过的) */
    uint32_t total_bytes;   /* 清单声明总字节 */
    int      done_files;
    int      total_files;
} am_mf_prog_t;

/* 返回码 */
#define AM_MF_OK            1   /* 全部文件就位, 安装标记已写 */
#define AM_MF_NO_MANIFEST   0   /* 清单拉不到/不合法 ⇒ 调用方应回退到 ZIP 下载 */
#define AM_MF_ERR_FILE     -1   /* 有文件反复失败(已完成的部分保留, 再点一次即续传) */
#define AM_MF_ERR_LAYOUT   -2   /* 建目录/写清单失败 */
#define AM_MF_ERR_NOMEM    -3   /* PSRAM 不足(清单或文件表) */
#define AM_MF_ERR_MARK     -4   /* 文件都下完了, 但写安装标记失败 */

/* 按清单安装。cb 可为 NULL; 它在**下载任务上下文**里被调用(不是中断),
 * 只该更新几个 volatile 状态字, 不要在里面做 IO。
 * mirror_base 形如 "https://bbk.linit.cn/bbk"(不带尾斜杠);
 * manifest_rel 形如 "pkgs/pal/manifest.json"。 */
int am_asset_multi_install(const char *mirror_base, const char *manifest_rel,
                           const char *target_dir, const char *zip_path,
                           void (*cb)(const am_mf_prog_t *));

#ifdef __cplusplus
}
#endif

#endif /* AM_ASSET_MULTI_H */
